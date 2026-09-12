// Network implementation, wifi + UDP to WIZ light controller
#include "net.h"

#include <stdio.h>
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "net";

#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASS "pass"
#define NVS_KEY_WIZ  "wiz_ip"

static QueueHandle_t s_light_q;
static TaskHandle_t s_net_task;
static int s_sock = -1;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static net_stats_t s_stats;
static char s_wiz_ip[16];
static bool s_light_on;

static void enqueue_light(bool on) {
    if (s_light_q == NULL) {
        return;
    }
    if (xQueueSend(s_light_q, &on, 0) != pdTRUE) {
        taskENTER_CRITICAL(&s_lock);
        s_stats.drops++;
        taskEXIT_CRITICAL(&s_lock);
    }
}

static esp_err_t nvs_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t nvs_load_str(const char *key, char *out, size_t out_sz) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NET_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t len = out_sz;
    err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    return err;
}

static esp_err_t nvs_store_str(const char *key, const char *value) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NET_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void ensure_socket(void) {
    if (s_sock >= 0) {
        return;
    }
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "udp socket failed");
    }
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        taskENTER_CRITICAL(&s_lock);
        s_stats.reconnects++;
        s_stats.state = 1;
        s_stats.ip[0] = '\0';
        taskEXIT_CRITICAL(&s_lock);
        ESP_LOGW(TAG, "sta disconnected; reconnecting");
        esp_wifi_connect();
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        char ip[16];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
        taskENTER_CRITICAL(&s_lock);
        s_stats.state = 2;
        memcpy(s_stats.ip, ip, sizeof(s_stats.ip));
        taskEXIT_CRITICAL(&s_lock);
        ensure_socket();
        ESP_LOGI(TAG, "got ip %s", ip);
    }
}

static void net_task(void *arg) {
    (void)arg;
    bool on;

    while (1) {
        if (xQueueReceive(s_light_q, &on, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        char dest_ip[16];
        int state;
        taskENTER_CRITICAL(&s_lock);
        state = s_stats.state;
        memcpy(dest_ip, s_wiz_ip, sizeof(dest_ip));
        taskEXIT_CRITICAL(&s_lock);

        if (state != 2 || dest_ip[0] == '\0' || s_sock < 0) {
            taskENTER_CRITICAL(&s_lock);
            s_stats.send_err++;
            taskEXIT_CRITICAL(&s_lock);
            continue;
        }

        struct sockaddr_in dest = { 0 };
        dest.sin_family = AF_INET;
        dest.sin_port = htons(WIZ_UDP_PORT);
        if (inet_pton(AF_INET, dest_ip, &dest.sin_addr) != 1) {
            taskENTER_CRITICAL(&s_lock);
            s_stats.send_err++;
            taskEXIT_CRITICAL(&s_lock);
            continue;
        }

        const char *payload = on ? WIZ_ON : WIZ_OFF;
        const int64_t t0 = esp_timer_get_time();
        const int n = sendto(s_sock, payload, strlen(payload), 0,
                             (struct sockaddr *)&dest, sizeof(dest));
        const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);

        taskENTER_CRITICAL(&s_lock);
        s_stats.send_last_us = us;
        if (us > s_stats.send_max_us) {
            s_stats.send_max_us = us;
        }
        if (n < 0) {
            s_stats.send_err++;
        } else {
            s_stats.sent++;
        }
        taskEXIT_CRITICAL(&s_lock);
    }
}

static esp_err_t wifi_start_sta(const char *ssid, const char *pass) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &event_handler, NULL, NULL));

    wifi_config_t wifi_config = { 0 };
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    taskENTER_CRITICAL(&s_lock);
    s_stats.state = 1;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "sta start ssid=%s", ssid);
    return ESP_OK;
}

esp_err_t net_start(void) {
    esp_err_t err = nvs_init();
    if (err != ESP_OK) {
        return err;
    }

    s_light_q = xQueueCreate(NET_LIGHT_QUEUE_LEN, sizeof(bool));
    if (s_light_q == NULL) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(net_task, "net", NET_TASK_STACK,
                                            NULL, NET_TASK_PRIO, &s_net_task,
                                            NET_TASK_CORE);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    char wiz[16] = { 0 };
    if (nvs_load_str(NVS_KEY_WIZ, wiz, sizeof(wiz)) == ESP_OK) {
        taskENTER_CRITICAL(&s_lock);
        memcpy(s_wiz_ip, wiz, sizeof(s_wiz_ip));
        memcpy(s_stats.wiz_ip, wiz, sizeof(s_stats.wiz_ip));
        taskEXIT_CRITICAL(&s_lock);
    }

    char ssid[33] = { 0 };
    char pass[65] = { 0 };
    if (nvs_load_str(NVS_KEY_SSID, ssid, sizeof(ssid)) != ESP_OK || ssid[0] == '\0') {
        ESP_LOGW(TAG, "no ssid in nvs; wifi disabled");
        return ESP_OK;
    }
    (void)nvs_load_str(NVS_KEY_PASS, pass, sizeof(pass));
    return wifi_start_sta(ssid, pass);
}

void net_set_light(bool on) {
    taskENTER_CRITICAL(&s_lock);
    s_light_on = on;
    taskEXIT_CRITICAL(&s_lock);
    enqueue_light(on);
}

bool net_send_light_now(bool on) {
    taskENTER_CRITICAL(&s_lock);
    s_light_on = on;
    taskEXIT_CRITICAL(&s_lock);
    if (s_light_q == NULL) {
        return false;
    }
    if (xQueueSend(s_light_q, &on, 0) != pdTRUE) {
        taskENTER_CRITICAL(&s_lock);
        s_stats.drops++;
        taskEXIT_CRITICAL(&s_lock);
        return false;
    }
    return true;
}

bool net_toggle_light(void) {
    bool next;
    taskENTER_CRITICAL(&s_lock);
    s_light_on = !s_light_on;
    next = s_light_on;
    taskEXIT_CRITICAL(&s_lock);
    enqueue_light(next);
    return next;
}

esp_err_t net_save_wifi(const char *ssid, const char *pass) {
    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > 32) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pass == NULL) {
        pass = "";
    }
    if (strlen(pass) > 63) {
        return ESP_ERR_INVALID_ARG;
    }
    // nvs_commit stalls flash/cache, ingest on core 1 may tick ovf once
    esp_err_t err = nvs_store_str(NVS_KEY_SSID, ssid);
    if (err != ESP_OK) {
        return err;
    }
    return nvs_store_str(NVS_KEY_PASS, pass);
}

esp_err_t net_save_wiz_ip(const char *ip) {
    if (ip == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    struct in_addr addr;
    if (inet_pton(AF_INET, ip, &addr) != 1) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = nvs_store_str(NVS_KEY_WIZ, ip);
    if (err != ESP_OK) {
        return err;
    }
    taskENTER_CRITICAL(&s_lock);
    strncpy(s_wiz_ip, ip, sizeof(s_wiz_ip) - 1);
    s_wiz_ip[sizeof(s_wiz_ip) - 1] = '\0';
    memcpy(s_stats.wiz_ip, s_wiz_ip, sizeof(s_stats.wiz_ip));
    taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t net_clear(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NET_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    (void)nvs_erase_key(h, NVS_KEY_SSID);
    (void)nvs_erase_key(h, NVS_KEY_PASS);
    (void)nvs_erase_key(h, NVS_KEY_WIZ);
    err = nvs_commit(h);
    nvs_close(h);
    taskENTER_CRITICAL(&s_lock);
    s_wiz_ip[0] = '\0';
    s_stats.wiz_ip[0] = '\0';
    taskEXIT_CRITICAL(&s_lock);
    return err;
}

void net_get_stats(net_stats_t *out) {
    if (out == NULL) {
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    *out = s_stats;
    memcpy(out->wiz_ip, s_wiz_ip, sizeof(out->wiz_ip));
    taskEXIT_CRITICAL(&s_lock);
    out->stack_hwm = (uint32_t)uxTaskGetStackHighWaterMark(s_net_task);
    out->rssi = 0;
    if (out->state == 2) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            out->rssi = ap.rssi;
        }
    }
}

void net_reset_stats(void) {
    taskENTER_CRITICAL(&s_lock);
    s_stats.sent = 0;
    s_stats.drops = 0;
    s_stats.send_err = 0;
    s_stats.reconnects = 0;
    s_stats.send_last_us = 0;
    s_stats.send_max_us = 0;
    taskEXIT_CRITICAL(&s_lock);
}
