#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define NET_TASK_STACK 4096
#define NET_TASK_PRIO 3         // prio is above CLI, below audio and the Wi-Fi driver
#define NET_TASK_CORE 0         // PRO_CPU with esp_wifi
#define NET_LIGHT_QUEUE_LEN 4
#define WIZ_UDP_PORT 38899
#define NET_NVS_NAMESPACE "jarvis"

#define WIZ_ON  "{\"method\":\"setPilot\",\"params\":{\"state\":true}}"
#define WIZ_OFF "{\"method\":\"setPilot\",\"params\":{\"state\":false}}"

typedef struct {
    // Network connection state
    int state;        // 0 unconfigured, 1 connecting, 2 got ip
    char ip[16];
    int8_t rssi;
    char wiz_ip[16];
    uint32_t sent;
    // Number of messages dropped from the light queue
    uint32_t drops;        // queue full
    uint32_t send_err;     // sendto < 0 or no ip
    uint32_t reconnects;
    uint32_t send_last_us;
    uint32_t send_max_us;
    uint32_t stack_hwm;
} net_stats_t;

esp_err_t net_start(void);
void net_set_light(bool on);
bool net_send_light_now(bool on);
esp_err_t net_save_wifi(const char *ssid, const char *pass);
esp_err_t net_save_wiz_ip(const char *ip);
esp_err_t net_clear(void);
void net_get_stats(net_stats_t *out);
void net_reset_stats(void);
