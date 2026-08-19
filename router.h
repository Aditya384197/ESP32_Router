#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_netif.h"

#define ROUTER_AP_IP "192.168.4.1"
#define ROUTER_AP_NETMASK "255.255.255.0"
#define ROUTER_DEFAULT_AP_SSID "ESP32_NAT_Router"
#define ROUTER_DEFAULT_WEB_PORT 80

extern esp_netif_t *g_ap_netif;
extern esp_netif_t *g_sta_netif;
extern bool g_sta_connected;
extern uint32_t g_ap_ip;

void router_save_config(void);
void router_factory_reset(void);
