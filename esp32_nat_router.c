/*
 * ESP32 NAT Router - Performance Edition
 *
 * Based on the NAT/routing architecture of Martin Ger's open-source
 * esp32_nat_router project, reduced to the classic ESP32 Wi-Fi NAT core.
 *
 * This build contains only the routing essentials plus:
 *   - AP hidden-SSID control
 *   - lightweight web management
 *   - HTTP OTA firmware update using the original synchronous rollback-confirmation pattern
 *
 * The packet-routing mechanism remains ESP-IDF/lwIP IPv4 forwarding + NAPT.
 * The web server is management-plane only and is deliberately small.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

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
#include "lwip/lwip_napt.h"
#include "lwip/ip4_addr.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "driver/gpio.h"

#include "router.h"

#define TAG "NAT"
#define NVS_NS "nat_perf"

#define KEY_STA_SSID "sta_ssid"
#define KEY_STA_PASS "sta_pass"
#define KEY_AP_SSID  "ap_ssid"
#define KEY_AP_PASS  "ap_pass"
#define KEY_AP_HIDDEN "ap_hidden"

#define MAX_STR 64
#define MAX_POST 512
#define RECONNECT_INITIAL_MS 500
#define RECONNECT_MAX_MS 10000

esp_netif_t *g_ap_netif = NULL;
esp_netif_t *g_sta_netif = NULL;
bool g_sta_connected = false;
uint32_t g_ap_ip = 0;

static char sta_ssid[MAX_STR] = "";
static char sta_pass[MAX_STR] = "";
static char ap_ssid[MAX_STR] = ROUTER_DEFAULT_AP_SSID;
static char ap_pass[MAX_STR] = "";
static bool ap_hidden = false;
static uint32_t reconnect_delay_ms = RECONNECT_INITIAL_MS;
static esp_timer_handle_t reconnect_timer;
static httpd_handle_t web_server = NULL;
static int64_t boot_us;

static void nvs_set_string(const char *key, const char *value)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, key, value);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_get_string(const char *key, char *out, size_t len, const char *def)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        strlcpy(out, def, len);
        return;
    }
    size_t required = len;
    esp_err_t err = nvs_get_str(h, key, out, &required);
    nvs_close(h);
    if (err != ESP_OK) strlcpy(out, def, len);
}

static void load_config(void)
{
    nvs_get_string(KEY_STA_SSID, sta_ssid, sizeof(sta_ssid), "");
    nvs_get_string(KEY_STA_PASS, sta_pass, sizeof(sta_pass), "");
    nvs_get_string(KEY_AP_SSID, ap_ssid, sizeof(ap_ssid), ROUTER_DEFAULT_AP_SSID);
    nvs_get_string(KEY_AP_PASS, ap_pass, sizeof(ap_pass), "");
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t hidden = 0;
        if (nvs_get_u8(h, KEY_AP_HIDDEN, &hidden) == ESP_OK) ap_hidden = hidden != 0;
        nvs_close(h);
    }
}

void router_save_config(void)
{
    nvs_set_string(KEY_STA_SSID, sta_ssid);
    nvs_set_string(KEY_STA_PASS, sta_pass);
    nvs_set_string(KEY_AP_SSID, ap_ssid);
    nvs_set_string(KEY_AP_PASS, ap_pass);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, KEY_AP_HIDDEN, ap_hidden ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

void router_factory_reset(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void reconnect_cb(void *arg)
{
    if (sta_ssid[0]) {
        esp_wifi_connect();
    }
    if (reconnect_delay_ms < RECONNECT_MAX_MS) {
        reconnect_delay_ms *= 2;
        if (reconnect_delay_ms > RECONNECT_MAX_MS)
            reconnect_delay_ms = RECONNECT_MAX_MS;
    }
}

static void schedule_reconnect(void)
{
    esp_timer_stop(reconnect_timer);
    esp_timer_start_once(reconnect_timer, (uint64_t)reconnect_delay_ms * 1000ULL);
}

static void set_ap_dns_from_sta(void)
{
    if (!g_ap_netif || !g_sta_netif) return;

    esp_netif_dns_info_t dns;
    if (esp_netif_get_dns_info(g_sta_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
        esp_netif_set_dns_info(g_ap_netif, ESP_NETIF_DNS_MAIN, &dns);
    }
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            if (sta_ssid[0]) esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            g_sta_connected = false;
            if (sta_ssid[0]) schedule_reconnect();
            break;

        case WIFI_EVENT_AP_START:
            if (g_ap_ip) ip_napt_enable(g_ap_ip, 1);
            break;

        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        g_sta_connected = true;
        reconnect_delay_ms = RECONNECT_INITIAL_MS;
        esp_timer_stop(reconnect_timer);
        set_ap_dns_from_sta();

        /* Re-assert NAPT after an uplink transition, matching the original
         * project's proven NAT lifecycle. */
        if (g_ap_ip) ip_napt_enable(g_ap_ip, 1);

        ESP_LOGI(TAG, "uplink IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static void configure_ap_netif(void)
{
    esp_netif_ip_info_t info = {0};
    info.ip.addr = esp_ip4addr_aton(ROUTER_AP_IP);
    info.gw.addr = info.ip.addr;
    info.netmask.addr = esp_ip4addr_aton(ROUTER_AP_NETMASK);
    g_ap_ip = info.ip.addr;

    esp_netif_dhcps_stop(g_ap_netif);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(g_ap_netif, &info));

    /* Offer the upstream DNS to AP clients once the STA is connected.
     * Until then, use a public resolver so a client has a usable default. */
    esp_netif_dns_info_t dns = {0};
    dhcps_offer_t dns_offer = OFFER_DNS;
    esp_netif_dhcps_option(g_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &dns_offer, sizeof(dns_offer));
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = esp_ip4addr_aton("1.1.1.1");
    esp_netif_set_dns_info(g_ap_netif, ESP_NETIF_DNS_MAIN, &dns);

    esp_netif_dhcps_start(g_ap_netif);
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    g_ap_netif = esp_netif_create_default_wifi_ap();
    g_sta_netif = esp_netif_create_default_wifi_sta();
    if (!g_ap_netif || !g_sta_netif) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi netifs");
        abort();
    }

    configure_ap_netif();

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                 &wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                 &wifi_event, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, ap_ssid, sizeof(ap.ap.ssid));
    strlcpy((char *)ap.ap.password, ap_pass, sizeof(ap.ap.password));
    ap.ap.channel = 0;
    ap.ap.max_connection = 8;
    ap.ap.beacon_interval = 100;
    ap.ap.ssid_hidden = ap_hidden ? 1 : 0;
    ap.ap.authmode = (strlen(ap_pass) >= 8) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, sta_ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, sta_pass, sizeof(sta.sta.password));
    sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));

    ESP_ERROR_CHECK(esp_wifi_start());

    /* Maximum configured TX power is left at the ESP-IDF default unless the
     * user explicitly changes it later; this avoids baking a regulatory value
     * into the firmware. */
    ESP_LOGI(TAG, "AP: %s (%s)", ap_ssid,
             ap_pass[0] ? "secured" : "open");

    if (!sta_ssid[0]) {
        ESP_LOGW(TAG, "No STA credentials configured; NAT will be idle until configured.");
    }
}

static char hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t url_decode(char *dst, size_t dst_len, const char *src)
{
    size_t n = 0;
    while (*src && n + 1 < dst_len) {
        if (*src == '+') {
            dst[n++] = ' ';
            src++;
        } else if (*src == '%' && src[1] && src[2] &&
                   hexval(src[1]) >= 0 && hexval(src[2]) >= 0) {
            dst[n++] = (char)((hexval(src[1]) << 4) | hexval(src[2]));
            src += 3;
        } else {
            dst[n++] = *src++;
        }
    }
    dst[n] = 0;
    return n;
}

static bool form_value(const char *body, const char *key, char *out, size_t out_len)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "%s=", key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, needle, strlen(needle)) == 0) {
            p += strlen(needle);
            const char *e = strchr(p, '&');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            char tmp[MAX_POST];
            if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
            memcpy(tmp, p, len);
            tmp[len] = 0;
            url_decode(out, out_len, tmp);
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return false;
}


static esp_err_t ota_upload_handler(httpd_req_t *req)
{
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "No OTA partition available");
    }

    if (req->content_len <= 0 || (size_t)req->content_len > update->size) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "Invalid firmware size");
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "OTA begin failed");
    }

    uint8_t *buf = malloc(4096);
    if (!buf) {
        esp_ota_abort(handle);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Out of memory");
    }

    int remaining = req->content_len;
    bool header_checked = false;

    while (remaining > 0) {
        int want = remaining > 4096 ? 4096 : remaining;
        int received = httpd_req_recv(req, (char *)buf, want);
        if (received <= 0) {
            free(buf);
            esp_ota_abort(handle);
            return ESP_FAIL;
        }

        if (!header_checked) {
            if (received < (int)sizeof(esp_image_header_t)) {
                free(buf);
                esp_ota_abort(handle);
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                           "Invalid firmware image");
            }

            const esp_image_header_t *hdr = (const esp_image_header_t *)buf;
            if (hdr->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
                free(buf);
                esp_ota_abort(handle);
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                           "Wrong firmware target");
            }
            header_checked = true;
        }

        err = esp_ota_write(handle, buf, received);
        if (err != ESP_OK) {
            free(buf);
            esp_ota_abort(handle);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "OTA write failed");
        }

        remaining -= received;
    }

    free(buf);

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "Firmware validation failed");
    }

    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Boot partition update failed");
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Firmware accepted. Rebooting...");
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
    return ESP_OK;
}

static const httpd_uri_t ota_upload = {
    .uri = "/ota",
    .method = HTTP_POST,
    .handler = ota_upload_handler,
    .user_ctx = NULL
};

static esp_err_t send_html(httpd_req_t *req)
{
    char *page = malloc(7800);
    if (!page) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Out of memory");
    }

    int written = snprintf(page, 7800,
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>ESP32 NAT Router</title>"
        "<style>"
        "body{margin:0;background:#fff;color:#111;font:15px system-ui,-apple-system,Segoe UI,sans-serif}"
        "main{max-width:760px;margin:42px auto;padding:0 22px}"
        "h1{font-size:28px;margin:0 0 6px;font-weight:650}"
        "p{color:#555}.card{border:1px solid #ddd;border-radius:14px;padding:20px;margin:18px 0}"
        "label{display:block;font-weight:600;margin:14px 0 6px}"
        "input{width:100%%;box-sizing:border-box;padding:11px 12px;border:1px solid #bbb;border-radius:9px;font:inherit}"
        "button{margin-top:18px;padding:11px 16px;border:0;border-radius:9px;background:#111;color:#fff;font:inherit;cursor:pointer}"
        "code{font-family:ui-monospace,monospace}.muted{color:#777;font-size:13px}"
        "</style></head><body><main>"
        "<h1>ESP32 NAT Router</h1>"
        "<p>Minimal management interface. Packet routing uses lwIP IPv4 NAPT.</p>"
        "<div class='card'><strong>Status</strong><p>"
        "Uplink: <code>%s</code><br>"
        "AP IP: <code>192.168.4.1</code><br>"
        "Free heap: <code>%lu bytes</code><br>"
        "Uptime: <code>%llu s</code>"
        "</p></div>"
        "<form class='card' method='post' action='/save'>"
        "<strong>Wi-Fi</strong>"
        "<label>Upstream SSID</label><input name='sta_ssid' maxlength='63' value='%s'>"
        "<label>Upstream password</label><input name='sta_pass' type='password' maxlength='63'>"
        "<label>Router SSID</label><input name='ap_ssid' maxlength='63' value='%s'>"
        "<label>Router password (8+ chars enables WPA2)</label><input name='ap_pass' type='password' maxlength='63'>"
        "<label><input style='width:auto;margin-right:8px' type='checkbox' name='ap_hidden' value='1' %s>Hide router SSID</label>"
        "<button type='submit'>Save &amp; reboot</button>"
        "<p class='muted'>Settings are stored in NVS.</p>"
        "</form>"
        "<div class='card'><strong>Firmware</strong><input id='fw' type='file' accept='.bin,application/octet-stream' required>"
        "<button type='button' onclick='uploadFirmware()'>Update firmware</button>"
        "<p id='otaMsg' class='muted'>Upload a firmware image built for this ESP32.</p></div>"
        "<div class='card'><strong>Recovery</strong><form method='post' action='/reset'>"
        "<button type='submit'>Factory reset &amp; reboot</button></form></div>"
        "<script>"
        "async function uploadFirmware(){"
        "const f=document.getElementById('fw').files[0],m=document.getElementById('otaMsg');"
        "if(!f){m.textContent='Select a firmware file first.';return;}"
        "if(!confirm('Upload and reboot the router?'))return;"
        "m.textContent='Uploading...';"
        "try{"
        "const r=await fetch('/ota',{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:f});"
        "m.textContent=await r.text();"
        "}catch(e){m.textContent='Upload failed.';}"
        "}</script></main></body></html>",
        g_sta_connected ? "connected" : (sta_ssid[0] ? "disconnected" : "not configured"),
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long long)((esp_timer_get_time() - boot_us) / 1000000LL),
        sta_ssid, ap_ssid, ap_hidden ? "checked" : "");

    if (written < 0 || written >= 7800) {
        free(page);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Page too large");
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t send_err = httpd_resp_send(req, page, written);
    free(page);
    return send_err;
}

static esp_err_t index_get(httpd_req_t *req)
{
    return send_html(req);
}

static esp_err_t save_post(httpd_req_t *req)
{
    char body[MAX_POST];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");

    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    body[got] = 0;

    char v[MAX_STR];
    if (form_value(body, "sta_ssid", v, sizeof(v))) strlcpy(sta_ssid, v, sizeof(sta_ssid));
    if (form_value(body, "sta_pass", v, sizeof(v))) strlcpy(sta_pass, v, sizeof(sta_pass));
    if (form_value(body, "ap_ssid", v, sizeof(v))) strlcpy(ap_ssid, v, sizeof(ap_ssid));
    if (form_value(body, "ap_pass", v, sizeof(v))) strlcpy(ap_pass, v, sizeof(ap_pass));
    ap_hidden = form_value(body, "ap_hidden", v, sizeof(v)) && strcmp(v, "1") == 0;

    if (ap_ssid[0] == 0 || strlen(ap_ssid) > 32 || strlen(sta_ssid) > 32 ||
        strlen(ap_pass) > 63 || strlen(sta_pass) > 63) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid Wi-Fi values");
    }

    router_save_config();
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Saved. Rebooting...");
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
    return ESP_OK;
}

static esp_err_t reset_post(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Factory reset. Rebooting...");
    vTaskDelay(pdMS_TO_TICKS(250));
    router_factory_reset();
    esp_restart();
    return ESP_OK;
}

static void web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = ROUTER_DEFAULT_WEB_PORT;
    cfg.max_open_sockets = 3;
    cfg.stack_size = 4096;
    cfg.recv_wait_timeout = 3;
    cfg.send_wait_timeout = 3;
    cfg.lru_purge_enable = true;

    if (httpd_start(&web_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Web server failed to start");
        return;
    }

    httpd_uri_t root = {
        .uri = "/", .method = HTTP_GET, .handler = index_get, .user_ctx = NULL
    };
    httpd_uri_t save = {
        .uri = "/save", .method = HTTP_POST, .handler = save_post, .user_ctx = NULL
    };
    httpd_uri_t reset = {
        .uri = "/reset", .method = HTTP_POST, .handler = reset_post, .user_ctx = NULL
    };
    httpd_register_uri_handler(web_server, &root);
    httpd_register_uri_handler(web_server, &save);
    httpd_register_uri_handler(web_server, &reset);
    httpd_register_uri_handler(web_server, &ota_upload);
}

static void factory_button_task(void *arg)
{
    /* Classic ESP32 BOOT button: GPIO0. This is a recovery mechanism only. */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io);

    int held = 0;
    while (true) {
        if (gpio_get_level(GPIO_NUM_0) == 0) {
            held += 100;
            if (held >= 5000) {
                ESP_LOGW(TAG, "Factory reset requested");
                router_factory_reset();
                esp_restart();
            }
        } else {
            held = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    boot_us = esp_timer_get_time();

    esp_reset_reason_t reset_reason = esp_reset_reason();
    ESP_LOGI(TAG, "reset reason: %d", (int)reset_reason);
    ESP_LOGI(TAG, "boot free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    load_config();

    const esp_timer_create_args_t timer_args = {
        .callback = reconnect_cb,
        .name = "sta_reconnect"
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &reconnect_timer));

    wifi_init();

    /* Preserve the original NAT mechanism: enable IPv4 NAPT on the AP IP. */
    ip_napt_enable(g_ap_ip, 1);

    web_start();
    xTaskCreate(factory_button_task, "factory_reset", 2048, NULL, 1, NULL);

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t confirm_err = esp_ota_mark_app_valid_cancel_rollback();
        if (confirm_err != ESP_OK) {
            /* Do not turn a rollback-state bookkeeping failure into a panic.
             * If the image is still pending verification, the bootloader can
             * safely fall back on the next reset. */
            ESP_LOGE(TAG, "OTA image confirmation failed: %s", esp_err_to_name(confirm_err));
        } else {
            ESP_LOGI(TAG, "OTA image confirmed valid");
        }
    }

    ESP_LOGI(TAG, "Performance edition started. NAPT=%s, free heap=%lu",
             "enabled", (unsigned long)esp_get_free_heap_size());

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
