// CLI with stats, log, reset commands
#include "cli.h"
#include "audio_dsp.h"
#include "audio_ingest.h"
#include "net.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

static const char *TAG = "cli";

static TaskHandle_t s_cli_task;

static void trim_line(char *line) {
    char *start = line;
    while (*start != '\0' && isspace((unsigned char)*start)) {
        start++;
    }
    if (start != line) {
        memmove(line, start, strlen(start) + 1);
    }
    size_t n = strlen(line);
    while (n > 0 && isspace((unsigned char)line[n - 1])) {
        line[--n] = '\0';
    }
}

static int split_args(char *line, char **argv, int max)
{
    int n = 0;
    char *p = line;
    while (*p != '\0' && n < max) {
        while (*p != '\0' && isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        argv[n++] = p;
        while (*p != '\0' && !isspace((unsigned char)*p)) {
            p++;
        }
        if (*p != '\0') {
            *p++ = '\0';
        }
    }
    return n;
}

static void cmd_help(void) {
    printf("help          list commands\r\n");
    printf("stats         dump timings, HWM, ovf, queue\r\n");
    printf("log on|off    1 Hz ingest log\r\n");
    printf("reset         zero max/min/ovf/drops/totals\r\n");
    printf("wifi <ssid> <pass>  save STA creds (no spaces in ssid)\r\n");
    printf("wiz <ip>      save bulb IPv4\r\n");
    printf("wiz on|off    send setPilot now\r\n");
    printf("net clear     erase wifi + wiz keys\r\n");
    printf("reboot        esp_restart (USB COM drops)\r\n");
}

static void cmd_stats(void) {
    audio_ingest_stats_t in;
    audio_dsp_stats_t dsp;
    net_stats_t net;
    audio_ingest_get_stats(&in);
    audio_dsp_get_stats(&dsp);
    net_get_stats(&net);

    const uint64_t uptime_us = (uint64_t)esp_timer_get_time();
    const uint32_t uptime_s = (uint32_t)(uptime_us / 1000000ULL);
    const uint32_t proc_avg = (in.blocks > 0)
                                  ? (uint32_t)(in.proc_total_us / in.blocks)
                                  : 0;
    const size_t heap_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t heap_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    printf("uptime_s=%" PRIu32 " heap_int=%u psram_free=%u\r\n",
           uptime_s, (unsigned)heap_int, (unsigned)heap_psram);
    printf("ingest blocks=%" PRIu32 " ovf=%" PRIu32
           " proc_us last=%" PRIu32 " avg=%" PRIu32 " max=%" PRIu32
           " period_us last=%" PRIu32 " min=%" PRIu32 " max=%" PRIu32
           " noise=%" PRIu64 " voiced=%d led=%d hwm=%" PRIu32 "\r\n",
           in.blocks, in.overrun,
           in.proc_last_us, proc_avg, in.proc_max_us,
           in.period_last_us, in.period_min_us, in.period_max_us,
           in.noise, in.voiced, in.led_on, in.stack_hwm);
    printf("dsp    blocks=%" PRIu32 " drops=%" PRIu32 " depth=%" PRIu32
           " proc_us last=%" PRIu32 " max=%" PRIu32 " hwm=%" PRIu32 "\r\n",
           dsp.blocks, dsp.drops, dsp.depth,
           dsp.proc_last_us, dsp.proc_max_us, dsp.stack_hwm);
    printf("wake   listen=%" PRIu32 " left_ms=%" PRIu32 " led=%" PRIu32
           " slices=%" PRIu32 " infers=%" PRIu32
           " infer_us last=%" PRIu32 " max=%" PRIu32
           " p_j=%" PRIu32 " p_on=%" PRIu32 " p_off=%" PRIu32
           " det_j=%" PRIu32 " det_on=%" PRIu32 " det_off=%" PRIu32
           " tmo=%" PRIu32 " rst=%" PRIu32 " arena=%" PRIu32 "\r\n",
           dsp.listening, dsp.listen_left_ms, dsp.led,
           dsp.slices, dsp.infers,
           dsp.infer_last_us, dsp.infer_max_us,
           dsp.prob_jarvis, dsp.prob_on, dsp.prob_off,
           dsp.det_jarvis, dsp.det_on, dsp.det_off,
           dsp.listen_timeouts, dsp.resets, dsp.arena_used);
    printf("net    state=%d ip=%s rssi=%d wiz=%s sent=%" PRIu32
           " drops=%" PRIu32 " send_err=%" PRIu32 " reconnects=%" PRIu32
           " send_us last=%" PRIu32 " max=%" PRIu32 " hwm=%" PRIu32 "\r\n",
           net.state, net.ip[0] ? net.ip : "-", (int)net.rssi,
           net.wiz_ip[0] ? net.wiz_ip : "-",
           net.sent, net.drops, net.send_err, net.reconnects,
           net.send_last_us, net.send_max_us, net.stack_hwm);
    printf("cli    hwm=%u\r\n", (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

static void dispatch(char *line) {
    // map line to command
    trim_line(line);
    if (line[0] == '\0') {
        return;
    }
    if (strcmp(line, "help") == 0) {
        cmd_help();
        return;
    }
    if (strcmp(line, "stats") == 0) {
        cmd_stats();
        return;
    }
    if (strcmp(line, "log on") == 0) {
        audio_ingest_set_log(true);
        printf("log on\r\n");
        return;
    }
    if (strcmp(line, "log off") == 0) {
        audio_ingest_set_log(false);
        printf("log off\r\n");
        return;
    }
    if (strcmp(line, "reset") == 0) {
        audio_ingest_reset_stats();
        audio_dsp_reset_stats();
        net_reset_stats();
        printf("ok\r\n");
        return;
    }

    char *argv[4];
    const int argc = split_args(line, argv, 4);

    if (argc == 3 && strcmp(argv[0], "wifi") == 0) {
        esp_err_t err = net_save_wifi(argv[1], argv[2]);
        if (err != ESP_OK) {
            printf("wifi save failed %s\r\n", esp_err_to_name(err));
            return;
        }
        printf("saved; reboot to apply\r\n");
        return;
    }
    if (argc == 2 && strcmp(argv[0], "wiz") == 0 &&
        strcmp(argv[1], "on") != 0 && strcmp(argv[1], "off") != 0) {
        esp_err_t err = net_save_wiz_ip(argv[1]);
        if (err != ESP_OK) {
            printf("wiz ip failed %s\r\n", esp_err_to_name(err));
            return;
        }
        printf("saved\r\n");
        return;
    }
    if (argc == 2 && strcmp(argv[0], "wiz") == 0) {
        const bool on = (strcmp(argv[1], "on") == 0);
        if (!on && strcmp(argv[1], "off") != 0) {
            printf("? wiz on|off|<ip>\r\n");
            return;
        }
        printf(net_send_light_now(on) ? "queued\r\n" : "queue full\r\n");
        return;
    }
    if (argc == 2 && strcmp(argv[0], "net") == 0 && strcmp(argv[1], "clear") == 0) {
        esp_err_t err = net_clear();
        printf(err == ESP_OK ? "cleared; reboot to apply wifi\r\n" : "clear failed\r\n");
        return;
    }
    if (argc == 1 && strcmp(argv[0], "reboot") == 0) {
        printf("rebooting\r\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
    }

    printf("? %s  (help)\r\n", line);
}

static void cli_task(void *arg) {
    (void)arg;
    char line[CLI_LINE_MAX];
    size_t len = 0;

    while (1) {
        uint8_t c;
        int n = usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY);
        if (n != 1) {
            continue;
        }

        if (c == '\r' || c == '\n') {
            if (len == 0) {
                continue;
            }
            line[len] = '\0';
            printf("\r\n");
            fflush(stdout);
            dispatch(line);
            fflush(stdout);
            len = 0;
            continue;
        }

        if (c == 0x08 || c == 0x7f) {
            if (len > 0) {
                len--;
                printf("\b \b");
                fflush(stdout);
            }
            continue;
        }

        if (c < 32 || c > 126) {
            continue;
        }
        if (len + 1 >= CLI_LINE_MAX) {
            continue;
        }
        line[len++] = (char)c;
        printf("%c", (char)c);
        fflush(stdout);
    }
}

esp_err_t cli_start(void) {
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    usb_serial_jtag_vfs_use_driver();
    setvbuf(stdout, NULL, _IONBF, 0);

    BaseType_t ok = xTaskCreatePinnedToCore(cli_task, "cli", CLI_TASK_STACK,
                                            NULL, CLI_TASK_PRIO, &s_cli_task,
                                            CLI_TASK_CORE);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "type 'help'");
    return ESP_OK;
}
