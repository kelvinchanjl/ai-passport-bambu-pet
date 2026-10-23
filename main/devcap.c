// main/devcap.c — 仅测试版(CONFIG_PET_TEST_SHORT_EXPIRY)。
// 输出格式(每行一条,和普通日志混在一起也能解析):
//   @FBBEGIN <tag> <w> <h>
//   @FB <y> <x1> <x2> <base64(RGB565 LE, (x2-x1+1)*2 字节)>
//   @FBEND <tag> <rows>
// 内存:只用一个行级 base64 缓冲(~700 B),像素直接从 LVGL 的部分刷新缓冲里读,不分配整帧。
#include "devcap.h"
#include "sdkconfig.h"
#if CONFIG_PET_TEST_SHORT_EXPIRY
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "mbedtls/base64.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "devcap";
static QueueHandle_t s_q;
static volatile bool s_cap;
static volatile int s_rows;
static char s_tag[16];
static unsigned char s_b64[244 * 2 * 4 / 3 + 8];
static bool s_row_seen[320];

static void rx_task(void *arg) {
    (void)arg;
    char line[32]; int n = 0; uint8_t c;
    for (;;) {
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) != 1) continue;
        if (c == '\r' || c == '\n') {
            if (n) { line[n] = 0; xQueueSend(s_q, line, 0); n = 0; }
        } else if (n < (int)sizeof line - 1) line[n++] = (char)c;
    }
}

static void flush_ev(lv_event_t *e) {
    if (!s_cap) return;
    lv_display_t *d = lv_event_get_target(e);
    const lv_area_t *a = lv_event_get_param(e);
    lv_draw_buf_t *b = lv_display_get_buf_active(d);
    if (!a || !b || !b->data) return;
    int w = a->x2 - a->x1 + 1;
    for (int y = a->y1; y <= a->y2; y++) {
        const uint8_t *row = b->data + (y - a->y1) * b->header.stride;
        size_t ol = 0;
        if (mbedtls_base64_encode(s_b64, sizeof s_b64, &ol, row, (size_t)w * 2) != 0) continue;
        s_b64[ol] = 0;
        printf("@FB %d %d %d %s\n", y, (int)a->x1, (int)a->x2, s_b64);
        if (y >= 0 && y < 320 && !s_row_seen[y]) { s_row_seen[y] = true; s_rows++; }
    }
    if (s_rows >= lv_display_get_vertical_resolution(d)) {
        s_cap = false;
        printf("@FBEND %s %d\n", s_tag, s_rows);
        fflush(stdout);
    }
}

void devcap_init(void) {
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 256; cfg.tx_buffer_size = 1024;
    if (usb_serial_jtag_driver_install(&cfg) == ESP_OK) usb_serial_jtag_vfs_use_driver();
    s_q = xQueueCreate(4, 32);
    xTaskCreate(rx_task, "devcap_rx", 2048, NULL, 3, NULL);
    lv_display_t *d = lv_display_get_default();
    if (d) lv_display_add_event_cb(d, flush_ev, LV_EVENT_FLUSH_START, NULL);   // 在圆角遮罩之后、字节交换之前
    ESP_LOGW(TAG, "TEST build serial commands: cap [tag] | demo1 | demo0 | demostep N | portal | reboot");
}

bool devcap_poll_cmd(char *out, int len) {
    char b[32];
    if (!s_q || xQueueReceive(s_q, b, 0) != pdTRUE) return false;
    snprintf(out, len, "%s", b);
    return true;
}

void devcap_capture(const char *tag) {
    lv_display_t *d = lv_display_get_default();
    if (!d || s_cap) return;
    snprintf(s_tag, sizeof s_tag, "%s", tag && tag[0] ? tag : "screen");
    memset(s_row_seen, 0, sizeof s_row_seen); s_rows = 0;
    printf("@FBBEGIN %s %d %d\n", s_tag, (int)lv_display_get_horizontal_resolution(d), (int)lv_display_get_vertical_resolution(d));
    s_cap = true;
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());   // LVGL 任务下一轮整屏重画,flush_ev 逐行打出
}
#else
void devcap_init(void) {}
bool devcap_poll_cmd(char *out, int len) { (void)out; (void)len; return false; }
void devcap_capture(const char *tag) { (void)tag; }
#endif
