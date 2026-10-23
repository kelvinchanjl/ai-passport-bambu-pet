// main/status_bar.c — v4.7 顶部状态条(时间 + 电池)。
// 时间:系统时间由 net.c 里的 SNTP(ntp.aliyun.com,TZ=CST-8)对时;对时前 time() 很小,显示 "--:--",绝不编时间。
//       对时一次后即使 Wi-Fi 断了,系统时钟继续走(到重启为止)。
// 电池:CW2017(bsp_battery),pet_app 任务里每 30 s 读一次(status_bar_poll,不在 LVGL 线程里),简单平滑;
//       连续读失败显示 "--%"。BSP 没有充电状态脚/寄存器接口,所以不画充电标记。
#include "status_bar.h"
#include <stdio.h>
#include <time.h>
#ifndef STATUS_BAR_PURE_ONLY   // host 单测只编下面两个纯函数
#include "bsp_battery.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

LV_FONT_DECLARE(font_pet_12);
#endif

#ifndef STATUS_BAR_PURE_ONLY
static const char *TAG = "sbar";
#define SB_TXT      0xD0D4DC
#define SB_DIM      0x5A6070
#define SB_GREEN    0x00BC44
#define SB_RED      0xFF5A5A
#define SB_READ_MS  30000
#define SB_LOG_MS   60000
#define T_VALID     1700000000

static lv_obj_t *s_time, *s_pct, *s_body, *s_fill, *s_tip;
static volatile int s_soc = -1, s_mv = -1;     // 后台任务写,LVGL 定时器读(int 读写是原子的)
static int s_shown_soc = -2, s_shown_min = -2;
static bool s_synced_logged;
#endif

void status_bar_fmt_time(const struct tm *tm, bool t_valid, char *out, int len) {
    if (!t_valid || !tm) snprintf(out, len, "--:--");
    else snprintf(out, len, "%02d:%02d", tm->tm_hour, tm->tm_min);
}

int status_bar_smooth(int prev, int raw) {
    if (raw < 0 || raw > 100) return prev;           // 读失败:保持上一次(失败计数由调用方处理)
    if (prev < 0) return raw;
    int s = (prev * 3 + raw + 2) / 4;                // 指数平滑 1/4
    if (s == prev && raw != prev) s += raw > prev ? 1 : -1;   // 保证最终能走到真实值
    return s;
}

#ifndef STATUS_BAR_PURE_ONLY
// v4.7:不单独开任务(省 ~3.4 KB 栈+TCB,min_heap 要 >= 40 KB),由 pet_app 任务每轮调一次,
// 内部限频 30 s;I2C 只在 pet_app 里读,不在 LVGL 线程里。
static esp_err_t s_bat_err = ESP_FAIL;
static bool s_bat_tried;
static int s_smooth = -1, s_fails;
static int64_t s_next_read_us, s_next_log_us;
void status_bar_poll(void) {
    int64_t now = esp_timer_get_time();
    if (now < s_next_read_us) return;
    s_next_read_us = now + (int64_t)SB_READ_MS * 1000;
    if (!s_bat_tried || s_bat_err != ESP_OK) {      // 首次初始化;没应答就下个周期再试
        s_bat_err = bsp_battery_init();
        if (!s_bat_tried) ESP_LOGI(TAG, "battery init: %s", esp_err_to_name(s_bat_err));
        s_bat_tried = true;
    }
    int raw = s_bat_err == ESP_OK ? bsp_battery_soc() : -1;
    int mv = s_bat_err == ESP_OK ? bsp_battery_mv() : -1;
    if (raw < 0 || raw > 100) { if (++s_fails >= 3) s_smooth = -1; }
    else { s_fails = 0; s_smooth = status_bar_smooth(s_smooth, raw); }
    s_soc = s_smooth; s_mv = mv;
    if (now >= s_next_log_us) {
        s_next_log_us = now + (int64_t)SB_LOG_MS * 1000;
        time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
        char ts[8]; status_bar_fmt_time(&tm, t > T_VALID, ts, sizeof ts);
        ESP_LOGI(TAG, "batt raw=%d smooth=%d mv=%d | time=%s | heap free=%u min=%u | stack_hw=%u", raw, s_smooth, mv, ts,
                 (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
}

static void set_batt(int soc) {
    if (soc == s_shown_soc) return;
    s_shown_soc = soc;
    if (soc < 0) {
        lv_label_set_text(s_pct, "--%");
        lv_obj_add_flag(s_fill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_border_color(s_body, lv_color_hex(SB_DIM), 0);
        lv_obj_set_style_bg_color(s_tip, lv_color_hex(SB_DIM), 0);
        return;
    }
    lv_label_set_text_fmt(s_pct, "%d%%", soc);
    uint32_t col = soc <= 20 ? SB_RED : SB_GREEN;
    int w = (soc * 16 + 50) / 100; if (w < 1) w = 1;   // 内腔 16 px
    lv_obj_set_width(s_fill, w);
    lv_obj_set_style_bg_color(s_fill, lv_color_hex(col), 0);
    lv_obj_remove_flag(s_fill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_border_color(s_body, lv_color_hex(SB_TXT), 0);
    lv_obj_set_style_bg_color(s_tip, lv_color_hex(SB_TXT), 0);
}

static void tick(lv_timer_t *t) {   // LVGL 任务里,已持锁;只有值变了才改文字(局部重绘)
    (void)t;
    time_t now = time(NULL);
    bool valid = now > T_VALID;
    struct tm tm; localtime_r(&now, &tm);
    int key = valid ? tm.tm_hour * 60 + tm.tm_min : -1;
    if (key != s_shown_min) {
        s_shown_min = key;
        char b[8]; status_bar_fmt_time(&tm, valid, b, sizeof b);
        lv_label_set_text(s_time, b);
        if (valid && !s_synced_logged) { s_synced_logged = true; ESP_LOGI(TAG, "time synced: %04d-%02d-%02d %s CST", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, b); }
    }
    set_batt(s_soc);
}

static lv_obj_t *box(lv_obj_t *p, int x, int y, int w, int h, int r) {
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y); lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, r, 0);
    return o;
}

void status_bar_create(void) {
    setenv("TZ", "CST-8", 1); tzset();
    lv_obj_t *top = lv_layer_top();
    int lh = lv_font_get_line_height(&font_pet_12);

    s_time = lv_label_create(top);
    lv_obj_set_style_text_font(s_time, &font_pet_12, 0);
    lv_obj_set_style_text_color(s_time, lv_color_hex(SB_TXT), 0);
    lv_label_set_text(s_time, "--:--");
    lv_obj_set_pos(s_time, 18, SB_Y - lh / 2);

    // 电池:外框 20x10(内腔 16x6)+ 正极头 2x4,右边缘 x=220
    s_body = box(top, 198, SB_Y - 5, 20, 10, 2);
    lv_obj_set_style_border_width(s_body, 1, 0);
    lv_obj_set_style_border_color(s_body, lv_color_hex(SB_DIM), 0);
    s_tip = box(top, 218, SB_Y - 2, 2, 4, 1);
    lv_obj_set_style_bg_opa(s_tip, LV_OPA_COVER, 0);
    s_fill = box(top, 200, SB_Y - 3, 16, 6, 1);
    lv_obj_set_style_bg_opa(s_fill, LV_OPA_COVER, 0);

    s_pct = lv_label_create(top);
    lv_obj_set_style_text_font(s_pct, &font_pet_12, 0);
    lv_obj_set_style_text_color(s_pct, lv_color_hex(SB_TXT), 0);
    lv_obj_set_style_text_align(s_pct, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(s_pct, 36);
    lv_obj_set_pos(s_pct, 196 - 36, SB_Y - lh / 2);   // 右对齐到 x=196
    lv_label_set_text(s_pct, "--%");
    set_batt(-1);

    lv_timer_create(tick, 1000, NULL);   // 每秒只比较一下分钟/电量,变了才改
    tick(NULL);
}
#endif
