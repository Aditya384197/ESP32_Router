/*
 * ESP32 NAT Router - Performance + Stability edition
 *
 * Packet path : Wi-Fi driver -> lwIP IPv4 forwarding -> NAPT -> Wi-Fi driver
 * Management  : tiny web page on the access point (http://192.168.4.1)
 *               with exactly two settings groups:
 *                 1) Uplink Wi-Fi  (SSID + password of the router you use for internet)
 *                 2) Access point  (SSID + password of this router)
 * Recovery    : hold the BOOT button (GPIO0) for 5 s -> settings reset to defaults
 * Watchdogs   : uplink reconnect with back-off, gateway ping health check,
 *               reboot if the uplink stays dead or wedged.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/lwip_napt.h"
#include "ping/ping_sock.h"
#include "esp_http_server.h"
#include "driver/gpio.h"

#define TAG "NAT"

/* ---------------- defaults / limits ---------------- */
#define NVS_NS              "router"
#define KEY_STA_SSID        "sta_ssid"
#define KEY_STA_PASS        "sta_pass"
#define KEY_AP_SSID         "ap_ssid"
#define KEY_AP_PASS         "ap_pass"

#define DEF_AP_SSID         "ESP32_Router"
#define DEF_AP_PASS         "12345678"
#define AP_IP_STR           "192.168.4.1"
#define AP_NETMASK_STR      "255.255.255.0"
#define AP_MAX_CLIENTS      8
#define AP_DEFAULT_CHANNEL  1

#define RECONNECT_INITIAL_MS 1000
#define RECONNECT_MAX_MS     10000

#define HEALTH_PERIOD_S      20
#define HEALTH_FAIL_RECONNECT 4     /* 4 failed rounds  (~80 s)  -> reconnect uplink */
#define HEALTH_FAIL_REBOOT    12    /* 12 failed rounds (~4 min) -> reboot            */
#define NO_UPLINK_REBOOT_S    900   /* configured uplink absent for 15 min -> reboot  */

#define DHCPS_OFFER_DNS      0x02   /* same value as lwIP OFFER_DNS */
#define MAX_BODY             1024

typedef struct {
    char sta_ssid[33];
    char sta_pass[65];
    char ap_ssid[33];
    char ap_pass[65];
} router_cfg_t;

static router_cfg_t s_cfg;
static esp_netif_t *s_ap_netif  = NULL;
static esp_netif_t *s_sta_netif = NULL;
static uint32_t s_ap_ip = 0;

static volatile bool     s_sta_up = false;
static volatile uint32_t s_up_gen = 0;
static uint32_t          s_reconnect_ms = RECONNECT_INITIAL_MS;
static esp_timer_handle_t s_reconnect_timer = NULL;

static volatile bool     s_ping_done = false;
static volatile uint32_t s_ping_recv = 0;

/* =========================================================
 *  Persistent config (NVS)
 * ========================================================= */
static void set_defaults(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    strlcpy(s_cfg.ap_ssid, DEF_AP_SSID, sizeof(s_cfg.ap_ssid));
    strlcpy(s_cfg.ap_pass, DEF_AP_PASS, sizeof(s_cfg.ap_pass));
}

static void nvs_read_str(nvs_handle_t h, const char *key, char *out, size_t cap)
{
    char tmp[80];
    size_t len = sizeof(tmp);
    if (nvs_get_str(h, key, tmp, &len) == ESP_OK) {
        strlcpy(out, tmp, cap);
    }
}

static void cfg_load(void)
{
    set_defaults();
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_read_str(h, KEY_STA_SSID, s_cfg.sta_ssid, sizeof(s_cfg.sta_ssid));
        nvs_read_str(h, KEY_STA_PASS, s_cfg.sta_pass, sizeof(s_cfg.sta_pass));
        nvs_read_str(h, KEY_AP_SSID,  s_cfg.ap_ssid,  sizeof(s_cfg.ap_ssid));
        nvs_read_str(h, KEY_AP_PASS,  s_cfg.ap_pass,  sizeof(s_cfg.ap_pass));
        nvs_close(h);
    }
    /* A broken AP config would lock the user out: fall back to defaults. */
    if (s_cfg.ap_ssid[0] == '\0' || strlen(s_cfg.ap_pass) < 8) {
        strlcpy(s_cfg.ap_ssid, DEF_AP_SSID, sizeof(s_cfg.ap_ssid));
        strlcpy(s_cfg.ap_pass, DEF_AP_PASS, sizeof(s_cfg.ap_pass));
    }
}

static esp_err_t cfg_save(void)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_str(h, KEY_STA_SSID, s_cfg.sta_ssid);
    if (e == ESP_OK) e = nvs_set_str(h, KEY_STA_PASS, s_cfg.sta_pass);
    if (e == ESP_OK) e = nvs_set_str(h, KEY_AP_SSID,  s_cfg.ap_ssid);
    if (e == ESP_OK) e = nvs_set_str(h, KEY_AP_PASS,  s_cfg.ap_pass);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

static void cfg_factory_reset(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* =========================================================
 *  Uplink (STA) reconnect handling
 * ========================================================= */
static void reconnect_cb(void *arg)
{
    if (s_cfg.sta_ssid[0] && !s_sta_up) {
        (void)esp_wifi_connect();
    }
}

static void schedule_reconnect(void)
{
    if (!s_cfg.sta_ssid[0] || s_reconnect_timer == NULL) return;
    (void)esp_timer_stop(s_reconnect_timer);
    (void)esp_timer_start_once(s_reconnect_timer, (uint64_t)s_reconnect_ms * 1000ULL);
    s_reconnect_ms *= 2;
    if (s_reconnect_ms > RECONNECT_MAX_MS) s_reconnect_ms = RECONNECT_MAX_MS;
}

static void enable_napt(void)
{
    if (s_ap_ip != 0) {
        ip_napt_enable(s_ap_ip, 1);
    }
}

static void set_ap_dns_from_uplink(void)
{
    esp_netif_dns_info_t dns;
    memset(&dns, 0, sizeof(dns));
    if (esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK &&
        dns.ip.type == ESP_IPADDR_TYPE_V4 && dns.ip.u_addr.ip4.addr != 0) {
        (void)esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns);
    }
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            if (s_cfg.sta_ssid[0]) (void)esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            s_sta_up = false;
            schedule_reconnect();
            break;
        case WIFI_EVENT_AP_START:
            enable_napt();
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        s_sta_up = true;
        s_up_gen++;
        s_reconnect_ms = RECONNECT_INITIAL_MS;
        if (s_reconnect_timer) (void)esp_timer_stop(s_reconnect_timer);
        set_ap_dns_from_uplink();
        enable_napt();
        ESP_LOGI(TAG, "uplink up, IP " IPSTR, IP2STR(&ev->ip_info.ip));
    }
}

/* =========================================================
 *  Wi-Fi + network bring-up
 * ========================================================= */
static void ap_netif_setup(void)
{
    esp_netif_ip_info_t info;
    memset(&info, 0, sizeof(info));
    info.ip.addr      = esp_ip4addr_aton(AP_IP_STR);
    info.gw.addr      = info.ip.addr;
    info.netmask.addr = esp_ip4addr_aton(AP_NETMASK_STR);
    s_ap_ip = info.ip.addr;

    (void)esp_netif_dhcps_stop(s_ap_netif);
    if (esp_netif_set_ip_info(s_ap_netif, &info) != ESP_OK) {
        ESP_LOGE(TAG, "AP IP config failed");
    }

    /* Hand DNS to clients; public resolver until the uplink DNS is known. */
    uint8_t offer = DHCPS_OFFER_DNS;
    (void)esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                                 ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));
    esp_netif_dns_info_t dns;
    memset(&dns, 0, sizeof(dns));
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = esp_ip4addr_aton("1.1.1.1");
    (void)esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns);

    (void)esp_netif_dhcps_start(s_ap_netif);
}

static void wifi_bringup(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_ap_netif  = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_ap_netif == NULL || s_sta_netif == NULL) {
        ESP_LOGE(TAG, "netif creation failed");
        esp_restart();
    }
    ap_netif_setup();

    const esp_timer_create_args_t targs = {
        .callback = reconnect_cb,
        .name = "sta_reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_reconnect_timer));

    /* Buffer counts come from sdkconfig.defaults (Espressif high-throughput profile). */
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    /* India: channels 1-13. Fixed policy so an upstream router on ch 12/13 is reachable. */
    wifi_country_t country = {
        .cc = "IN",
        .schan = 1,
        .nchan = 13,
        .max_tx_power = 20,
        .policy = WIFI_COUNTRY_POLICY_MANUAL,
    };
    if (esp_wifi_set_country(&country) != ESP_OK) {
        ESP_LOGW(TAG, "country setup failed, using default");
    }

    wifi_config_t ap;
    memset(&ap, 0, sizeof(ap));
    size_t ap_len = strlen(s_cfg.ap_ssid);
    memcpy(ap.ap.ssid, s_cfg.ap_ssid, ap_len);
    ap.ap.ssid_len = (uint8_t)ap_len;
    strlcpy((char *)ap.ap.password, s_cfg.ap_pass, sizeof(ap.ap.password));
    ap.ap.channel = AP_DEFAULT_CHANNEL;   /* follows the uplink channel once connected */
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = AP_MAX_CLIENTS;
    ap.ap.beacon_interval = 100;

    wifi_config_t sta;
    memset(&sta, 0, sizeof(sta));
    size_t sta_len = strlen(s_cfg.sta_ssid);
    memcpy(sta.sta.ssid, s_cfg.sta_ssid, sta_len);
    strlcpy((char *)sta.sta.password, s_cfg.sta_pass, sizeof(sta.sta.password));
    sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;          /* pick the strongest matching AP */
    sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta.sta.threshold.rssi = -127;
    sta.sta.threshold.authmode = s_cfg.sta_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    sta.sta.pmf_cfg.capable = true;
    sta.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Always-on radio: lowest latency, highest throughput. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    enable_napt();
}

/* =========================================================
 *  Health watchdog: gateway ping + uplink-dead reboot
 * ========================================================= */
static void ping_end_cb(esp_ping_handle_t hdl, void *args)
{
    uint32_t recv = 0;
    (void)esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &recv, sizeof(recv));
    s_ping_recv = recv;
    s_ping_done = true;
    (void)esp_ping_delete_session(hdl);
}

/* true = gateway answered (or test not possible), false = 3/3 lost */
static bool gateway_alive(void)
{
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_sta_netif, &ip) != ESP_OK || ip.gw.addr == 0) {
        return true;
    }

    esp_ping_config_t pc = ESP_PING_DEFAULT_CONFIG();
    pc.count = 3;
    pc.interval_ms = 1000;
    pc.timeout_ms = 1000;
    pc.data_size = 32;
    pc.task_stack_size = 3072;
    pc.task_prio = 3;
    ip_addr_set_ip4_u32(&pc.target_addr, ip.gw.addr);

    esp_ping_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.on_ping_end = ping_end_cb;

    s_ping_done = false;
    s_ping_recv = 0;
    esp_ping_handle_t h = NULL;
    if (esp_ping_new_session(&pc, &cbs, &h) != ESP_OK) return true;
    if (esp_ping_start_session(h) != ESP_OK) {
        (void)esp_ping_delete_session(h);
        return true;
    }
    for (int i = 0; i < 100 && !s_ping_done; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!s_ping_done) return true;      /* inconclusive: do not act */
    return s_ping_recv > 0;
}

static void health_task(void *arg)
{
    uint32_t down_s = 0;
    uint32_t gen = 0;
    int fails = 0;
    bool armed = false;   /* only act on ping failures after the gateway answered once */

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(HEALTH_PERIOD_S * 1000));

        if (s_cfg.sta_ssid[0] == '\0') continue;

        if (!s_sta_up) {
            armed = false;
            fails = 0;
            down_s += HEALTH_PERIOD_S;
            if (down_s >= NO_UPLINK_REBOOT_S) {
                ESP_LOGE(TAG, "uplink absent too long, rebooting");
                esp_restart();
            }
            continue;
        }
        down_s = 0;

        if (gen != s_up_gen) {          /* fresh connection */
            gen = s_up_gen;
            armed = false;
            fails = 0;
        }

        if (gateway_alive()) {
            armed = true;
            fails = 0;
        } else if (armed) {
            fails++;
            if (fails == HEALTH_FAIL_RECONNECT) {
                ESP_LOGW(TAG, "gateway unreachable, reconnecting uplink");
                (void)esp_wifi_disconnect();    /* reconnect follows via event */
            } else if (fails >= HEALTH_FAIL_REBOOT) {
                ESP_LOGE(TAG, "uplink wedged, rebooting");
                esp_restart();
            }
        }
    }
}

/* =========================================================
 *  Recovery button (BOOT / GPIO0 held 5 s)
 * ========================================================= */
static void button_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    int held_ms = 0;
    for (;;) {
        if (gpio_get_level(GPIO_NUM_0) == 0) {
            held_ms += 100;
            if (held_ms >= 5000) {
                ESP_LOGW(TAG, "reset to defaults");
                cfg_factory_reset();
                esp_restart();
            }
        } else {
            held_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* =========================================================
 *  Web UI
 * ========================================================= */
static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Extract one field from an application/x-www-form-urlencoded body. */
static void form_get(const char *body, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    const char *p = body;
    out[0] = '\0';
    while (*p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            size_t o = 0;
            while (*p && *p != '&') {
                char c = *p;
                if (c == '+') {
                    c = ' ';
                    p++;
                } else if (c == '%' && hexv(p[1]) >= 0 && hexv(p[2]) >= 0) {
                    c = (char)((hexv(p[1]) << 4) | hexv(p[2]));
                    p += 3;
                } else {
                    p++;
                }
                if (o + 1 < cap) out[o++] = c;
            }
            out[o] = '\0';
            return;
        }
        const char *amp = strchr(p, '&');
        if (amp == NULL) break;
        p = amp + 1;
    }
}

static void html_esc(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; *in; in++) {
        const char *rep = NULL;
        switch (*in) {
        case '&':  rep = "&amp;";  break;
        case '<':  rep = "&lt;";   break;
        case '>':  rep = "&gt;";   break;
        case '"':  rep = "&quot;"; break;
        case '\'': rep = "&#39;";  break;
        default: break;
        }
        if (rep) {
            size_t n = strlen(rep);
            if (o + n + 1 > cap) break;
            memcpy(out + o, rep, n);
            o += n;
        } else {
            if (o + 2 > cap) break;
            out[o++] = *in;
        }
    }
    out[o] = '\0';
}

static const char PAGE_HEAD[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>ESP32 Router</title><style>"
    "body{font-family:sans-serif;max-width:420px;margin:16px auto;padding:0 12px}"
    "input{width:100%;box-sizing:border-box;padding:9px;margin:4px 0 12px;font-size:16px}"
    "input[type=checkbox]{width:auto;margin:0 6px 0 0}"
    "button{width:100%;padding:12px;font-size:16px}"
    "fieldset{margin-bottom:14px}.s{color:#444;font-size:14px}"
    "</style></head><body><h2>ESP32 Router</h2>";

static esp_err_t root_get(httpd_req_t *req)
{
    char up[112];
    char line[320];
    char e_ss[400], e_sp[400], e_as[400], e_ap[400];
    esp_netif_ip_info_t ip;
    wifi_ap_record_t rec;
    wifi_sta_list_t list;
    int clients = 0;

    if (esp_wifi_ap_get_sta_list(&list) == ESP_OK) clients = list.num;

    if (!s_cfg.sta_ssid[0]) {
        strlcpy(up, "not configured", sizeof(up));
    } else if (s_sta_up && esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK) {
        int rssi = 0;
        if (esp_wifi_sta_get_ap_info(&rec) == ESP_OK) rssi = rec.rssi;
        snprintf(up, sizeof(up), "connected &middot; " IPSTR " &middot; %d dBm",
                 IP2STR(&ip.ip), rssi);
    } else {
        strlcpy(up, "connecting...", sizeof(up));
    }

    html_esc(s_cfg.sta_ssid, e_ss, sizeof(e_ss));
    html_esc(s_cfg.sta_pass, e_sp, sizeof(e_sp));
    html_esc(s_cfg.ap_ssid,  e_as, sizeof(e_as));
    html_esc(s_cfg.ap_pass,  e_ap, sizeof(e_ap));

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);

    snprintf(line, sizeof(line),
             "<p class=\"s\">Uplink: <b>%s</b><br>Clients: %d &middot; Uptime: %llu s"
             "<br>Free heap: %lu (min %lu)</p>",
             up, clients,
             (unsigned long long)(esp_timer_get_time() / 1000000LL),
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)esp_get_minimum_free_heap_size());
    httpd_resp_sendstr_chunk(req, line);

    httpd_resp_sendstr_chunk(req,
        "<form method=\"post\" action=\"/save\">"
        "<fieldset><legend>Uplink Wi-Fi (internet source)</legend>"
        "Name (SSID)<input name=\"ss\" maxlength=\"32\" value=\"");
    httpd_resp_sendstr_chunk(req, e_ss);
    httpd_resp_sendstr_chunk(req,
        "\">Password (empty = open network)"
        "<input class=\"pw\" type=\"password\" name=\"sp\" maxlength=\"63\" value=\"");
    httpd_resp_sendstr_chunk(req, e_sp);
    httpd_resp_sendstr_chunk(req,
        "\"></fieldset>"
        "<fieldset><legend>Access point (this router)</legend>"
        "Name (SSID)<input name=\"as\" maxlength=\"32\" required value=\"");
    httpd_resp_sendstr_chunk(req, e_as);
    httpd_resp_sendstr_chunk(req,
        "\">Password (8-63 characters)"
        "<input class=\"pw\" type=\"password\" name=\"ap\" minlength=\"8\" maxlength=\"63\" required value=\"");
    httpd_resp_sendstr_chunk(req, e_ap);
    httpd_resp_sendstr_chunk(req,
        "\"></fieldset>"
        "<label><input type=\"checkbox\" onclick=\"var t=this.checked?'text':'password';"
        "var a=document.querySelectorAll('.pw');for(var i=0;i<a.length;i++)a[i].type=t\">"
        "Show passwords</label><br><br>"
        "<button type=\"submit\">Save &amp; restart</button></form></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t send_msg(httpd_req_t *req, const char *status, const char *msg)
{
    char page[1200];
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    snprintf(page, sizeof(page),
             "%s<p>%s</p><p><a href=\"/\">Back</a></p></body></html>", PAGE_HEAD, msg);
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t save_post(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > MAX_BODY) {
        return send_msg(req, "400 Bad Request", "Invalid request size.");
    }

    char body[MAX_BODY + 1];
    size_t got = 0;
    int timeouts = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 3) return ESP_FAIL;
            continue;
        }
        if (r <= 0) return ESP_FAIL;
        got += (size_t)r;
    }
    body[got] = '\0';

    char ss[130], sp[130], as[130], ap[130];
    form_get(body, "ss", ss, sizeof(ss));
    form_get(body, "sp", sp, sizeof(sp));
    form_get(body, "as", as, sizeof(as));
    form_get(body, "ap", ap, sizeof(ap));

    size_t l_ss = strlen(ss), l_sp = strlen(sp), l_as = strlen(as), l_ap = strlen(ap);
    if (l_ss > 32)                        return send_msg(req, "400 Bad Request", "Uplink name must be at most 32 characters.");
    if (l_sp != 0 && (l_sp < 8 || l_sp > 63)) return send_msg(req, "400 Bad Request", "Uplink password must be 8-63 characters (or empty for an open network).");
    if (l_as < 1 || l_as > 32)            return send_msg(req, "400 Bad Request", "Access point name must be 1-32 characters.");
    if (l_ap < 8 || l_ap > 63)            return send_msg(req, "400 Bad Request", "Access point password must be 8-63 characters.");

    strlcpy(s_cfg.sta_ssid, ss, sizeof(s_cfg.sta_ssid));
    strlcpy(s_cfg.sta_pass, sp, sizeof(s_cfg.sta_pass));
    strlcpy(s_cfg.ap_ssid,  as, sizeof(s_cfg.ap_ssid));
    strlcpy(s_cfg.ap_pass,  ap, sizeof(s_cfg.ap_pass));

    if (cfg_save() != ESP_OK) {
        return send_msg(req, "500 Internal Server Error", "Could not save settings.");
    }

    send_msg(req, "200 OK", "Saved. The router is restarting - reconnect to the access point and open http://" AP_IP_STR);
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

static void web_start(void)
{
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.stack_size = 6144;
    hc.max_open_sockets = 4;
    hc.lru_purge_enable = true;
    hc.recv_wait_timeout = 5;
    hc.send_wait_timeout = 5;
    hc.task_priority = 4;

    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &hc) != ESP_OK) {
        ESP_LOGE(TAG, "web server start failed");
        return;
    }
    httpd_uri_t root = { .uri = "/",     .method = HTTP_GET,  .handler = root_get,  .user_ctx = NULL };
    httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_post, .user_ctx = NULL };
    httpd_register_uri_handler(srv, &root);
    httpd_register_uri_handler(srv, &save);
}

/* =========================================================
 *  Entry point
 * ========================================================= */
void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    cfg_load();
    wifi_bringup();
    web_start();

    xTaskCreate(health_task, "health", 3072, NULL, 3, NULL);
    xTaskCreate(button_task, "button", 2048, NULL, 1, NULL);

    ESP_LOGI(TAG, "router up, free heap %lu", (unsigned long)esp_get_free_heap_size());
}
