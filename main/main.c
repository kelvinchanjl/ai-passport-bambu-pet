// main/main.c — 拓竹桌面小宠物:初始化、模式切换(宠物/配网/重新登录/恢复出厂)、按键分发。
//
// v4.4 按键(中键 = 确定键 BSP_BTN_OK):
//   上/下 = 切页 首页 → 详情页 → 耗材页 → 首页(循环);耗材页一屏放不下时先页内翻,到头/到底再切页。
//   双击 中 = 下一台打印机(双击窗口 250 ms;只在 >= 2 台且不在演示模式时判定,否则单击立即生效)。
//   单击 中 = 首页「完成」先收下;否则下一页。长按 中 = 设置页(演示模式开关 / 重新配网 / 恢复出厂 / 返回);
//   设置页里 上/下 选择、中 确定;在「恢复出厂设置」上按住中键 1 秒出确认页,继续按住 5 秒清除配置。
//   演示模式:单击 中 = 下一个状态(6 个,n/6),不自动轮换;上/下照样切三页,耗材页是示例机器的 9 个槽;不换机器。
//   底部提示只在详情/耗材页、常亮;首页没有提示(布局同 v4.3);演示角标「n/6」只在首页。
//   断料/失败/完成的机器:停 3 秒后自动跳过去(pet_switch_t 防抖);演示模式里不跳。
// v4.8 休眠(熄屏):无操作 2/5 分钟(设置页可选 关/2/5,默认 2)熄屏;熄屏画面 A 全黑(默认)/ B 时钟 / C 线线。
//   A:背光 0 → DISPOFF+SLPIN → 暂停 LVGL 刷屏;醒来 SLPOUT → 恢复刷屏 → 让 taskLVGL 刷一帧 → DISPON → 背光 80。
//   B/C:不 SLPIN,背光 8%,独立熄屏页面每 60 s 刷新;醒来删页面、背光 80。Wi-Fi/MQTT 不受影响。
//   熄屏时任意键按下只亮屏(key_wake_guard:整次按键连同之后 250 ms 全吞);打印「变成」完成/失败/暂停/断料时自动亮屏,
//   暂停时切到那台。配网/二维码/热点/重新登录/恢复出厂确认/演示模式不熄屏。设置页开着时到点:先关设置页再熄屏。
#include "app_config.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "key_guard.h"
#include "sleep_logic.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "pet_logic.h"
#include "pet_ui.h"
#include "status_bar.h"
#include "devcap.h"
#include "lvgl.h"
#include "setup_portal.h"
#include "status_source.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if CONFIG_PET_DEBUG_STATUS_INJECT && !CONFIG_PET_TEST_SHORT_EXPIRY
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "mbedtls/base64.h"
#endif

static const char *TAG = "pet";

// v4.2:开机复位原因(名字+编号)+ NVS 开机计数与上次原因,串口抓日志时能看出中途有没有重启、为什么
static const char *reset_name(esp_reset_reason_t r) {
    switch (r) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXT";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    case ESP_RST_USB: return "USB_UART_CHIP_RESET";
    case ESP_RST_JTAG: return "JTAG";
    case ESP_RST_EFUSE: return "EFUSE";
    case ESP_RST_PWR_GLITCH: return "PWR_GLITCH";
    case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
    default: return "UNKNOWN";
    }
}
static void log_boot_reason(void) {
    esp_reset_reason_t r = esp_reset_reason();
    uint32_t count = 0, freset = 0; int32_t prev = -1;
    nvs_handle_t h;
    if (nvs_open("petboot", NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, "count", &count);
        nvs_get_u32(h, "freset", &freset);
        nvs_get_i32(h, "last", &prev);
        count++;
        nvs_set_u32(h, "count", count);
        nvs_set_i32(h, "last", (int32_t)r);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI("boot", "reset reason=%s(%d) boot#%lu prev=%s(%ld)", reset_name(r), (int)r, (unsigned long)count,
             prev < 0 ? "NONE" : reset_name((esp_reset_reason_t)prev), (long)prev);
    if (freset) ESP_LOGW("boot", "last factory reset (petcfg erased) at boot#%lu", (unsigned long)freset);
}


typedef struct { bsp_btn_t btn; bsp_btn_ev_t ev; } key_ev_t;
static QueueHandle_t s_keys;
static app_config_t s_cfg;
static bool s_cloud_ok;
static pet_machine_t s_machines[PET_MAX_PRINTERS];
#define s_machine s_machines[status_store_current()]
// 打印用时:每台机器的开始时间按 job_id 存 NVS("petjob"/"job","job1".."job3"),重启后同一 job 仍能显示「用时」
static pet_job_t s_jobs[PET_MAX_PRINTERS];
static pet_switch_t s_switch;
static int s_set_sel;                // 设置页当前选中项
static pet_ams_t s_ams_buf;          // v4.3 当前机器的 AMS(从存储拷出来再交给 UI)

static void job_load(void) {
    nvs_handle_t h;
    bool open = nvs_open("petjob", NVS_READONLY, &h) == ESP_OK;
    for (int i = 0; i < PET_MAX_PRINTERS; i++) {
        pet_job_init(&s_jobs[i]);
        size_t n = sizeof s_jobs[i];
        const char *key = pet_job_key(i);   // v4.8:越界返回 NULL(原 job_key 的 K[i] 在 i>=4 时越界读)
        if (open && key && (nvs_get_blob(h, key, &s_jobs[i], &n) != ESP_OK || n != sizeof s_jobs[i])) pet_job_init(&s_jobs[i]);
        s_jobs[i].job[sizeof s_jobs[i].job - 1] = 0;
    }
    if (open) nvs_close(h);
}
static void job_save(int i) {
    const char *key = pet_job_key(i);
    if (!key) return;
    nvs_handle_t h;
    if (nvs_open("petjob", NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, key, &s_jobs[i], sizeof s_jobs[i]) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}
static pet_conn_t s_conn;             // v4.4.1 当前机器的「连接中/未连上」计时
static pet_okkey_t s_ok;             // v4.4 中键单击/双击/长按状态机(pet_logic.c,主机测试 test_keys_v44)
static key_guard_t s_key_guard;      // 本固件不深睡;保留守卫以兼容以后加休眠
static volatile bool s_input_ready;
static pet_reset_t s_rst;             // v4.5.3 恢复出厂防误触状态机
static bool s_ok_held;                // 中键物理上是否按着(PRESS/RELEASE 跟踪)
static bool s_portal_on;
// ---- v4.8 休眠 ----
static key_wake_guard_t s_wake_guard;   // 只在按键回调(esp_timer 任务)里读写
static volatile bool s_asleep;          // main 任务写:先置位再关背光
static volatile bool s_wake_req;        // 按键回调写:熄屏时按下 → 请 main 任务亮屏
static pet_sleep_t s_slp;
static uint8_t s_slp_face;              // 这次熄屏用的画面
static ui_screen_t s_slp_screen;        // 熄屏时的页面;页面被别处换掉(到期提醒/重新登录)就亮屏
static pet_state_t s_wake_prev[PET_MAX_PRINTERS];
static bool s_valid_cache[PET_MAX_PRINTERS];   // 这台有没有有效报文(MQTT 重连时 bambu_cloud 会 reset_all → valid=false)
static int s_pct_cache[PET_MAX_PRINTERS];   // 每台进度(B 画面用;主循环里顺手记下,不再多拷一份 printer_status_t)
static int s_slp_key, s_slp_refresh_n, s_slp_shift_pos;
static uint32_t s_slp_ms;
static pet_sleep_info_t s_slp_info;     // 静态:不占 pet_app 栈
static printer_info_t s_slp_pinfo;

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) return;
    // v4.8:熄屏时按下即拦(在进队列之前),这次按键连同松开后 250 ms 的 CLICK/DOUBLE/LONG 全吞,只亮屏
    bool wake = false;
    kwg_ev_t ke = ev == BSP_BTN_PRESS ? KWG_EV_PRESS : ev == BSP_BTN_RELEASE ? KWG_EV_RELEASE : KWG_EV_OTHER;
    if (key_wake_guard_consume(&s_wake_guard, (int)btn, ke, s_asleep, esp_timer_get_time(), &wake)) {
        if (wake) s_wake_req = true;
        return;
    }
    if (!s_input_ready) return;
    key_ev_t k = { btn, ev };
    xQueueSend(s_keys, &k, 0);  // 非阻塞,满了就丢
}

static void ui_screen(ui_screen_t sc, const char *arg) {
    if (bsp_lvgl_lock(200)) { pet_ui_set_screen(sc, arg); bsp_lvgl_unlock(); }
}

static bool s_portal_cancellable;     // 已配置 + 非 reauth:扫码页可长按退出
static bool s_portal_return_settings; // 退出后回到设置页(进配网前)
static void header_refresh(bool animate, int dir);
static void settings(bool on);

static void start_portal(bool reauth) {
    if (s_portal_on) return;
    pet_portal_exit_plan_t plan = pet_portal_exit_plan(s_cloud_ok, reauth);
    s_portal_cancellable = plan.allow;
    s_portal_return_settings = plan.restore_prev_page;
    s_portal_on = setup_portal_start(&s_cfg, reauth);
    if (s_portal_on) {
        ui_screen(UI_SETUP_QR, setup_portal_ap_name());
        if (pet_ui_screen() != UI_SETUP_QR) {
            setup_portal_stop();
            s_portal_on = false;
            s_portal_cancellable = false;
            ESP_LOGE(TAG, "qr page failed (lvgl oom?), portal stopped");
            return;
        }
        if (bsp_lvgl_lock(200)) { pet_ui_qr_exit_hint(s_portal_cancellable); bsp_lvgl_unlock(); }
    }
}

static void stop_portal_to_prev(void) {
    if (!s_portal_on) return;
    setup_portal_stop();                  // 关热点/HTTP/DNS;已写入 NVS 的提交不回滚
    s_portal_on = false;
    s_portal_cancellable = false;
    ui_screen(UI_PET, NULL);
    if (s_portal_return_settings) { s_set_sel = SET_REPAIR; settings(true); }
    else header_refresh(false, 0);
    ESP_LOGI(TAG, "portal exit heap free=%u min=%u", (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
}

static void header_refresh(bool animate, int dir) {
    int n = status_store_count(), i = status_store_current();
    printer_info_t info; bool online;
    status_store_get_idx(i, NULL, NULL, &online, &info);
    if (!bsp_lvgl_lock(200)) return;
    if (animate) pet_ui_switch_printer(dir, info.name, info.product, online, i, n);
    else pet_ui_set_header(info.name, info.product, online, i, n);
    bsp_lvgl_unlock();
}

static void select_printer(int idx, int dir, bool manual) {
    int n = status_store_count();
    if (n <= 1) return;
    idx = ((idx % n) + n) % n;
    if (idx == status_store_current()) return;
    status_store_select(idx);
    pet_conn_init(&s_conn);              // 换机器:重新计 30 秒
    if (bsp_lvgl_lock(200)) { pet_ui_filament_reset_scroll(); bsp_lvgl_unlock(); }
    pet_switch_set_cur(&s_switch, idx, (uint32_t)(esp_timer_get_time() / 1000), manual);
    header_refresh(true, dir);
    ESP_LOGI(TAG, "printer -> #%d (%s)", idx, manual ? "key" : "auto");
}

static void set_demo(bool on) {
    if (on) { status_store_activate(SRC_DEMO); demo_source_start(); }
    else {
        demo_source_stop();
        if (s_cloud_ok) status_store_activate(SRC_CLOUD);
        else start_portal(false);
    }
    for (int i = 0; i < PET_MAX_PRINTERS; i++) pet_machine_init(&s_machines[i]);
    pet_switch_init(&s_switch, status_store_count());
    pet_okkey_init(&s_ok);
}

static void settings(bool on) {
    if (on) s_set_sel = 0;
    if (bsp_lvgl_lock(200)) { pet_ui_show_settings(on, s_set_sel, status_store_active() == SRC_DEMO, s_cfg.sleep_min, s_cfg.sleep_face); bsp_lvgl_unlock(); }
}
static void settings_redraw(void) {
    if (bsp_lvgl_lock(200)) { pet_ui_show_settings(true, s_set_sel, status_store_active() == SRC_DEMO, s_cfg.sleep_min, s_cfg.sleep_face); bsp_lvgl_unlock(); }
}

static int cur_page(void) { return pet_ui_filament_open() ? 2 : pet_ui_detail_open() ? 1 : 0; }

// v4.4:上下键切页 首页(0) → 详情(1) → 耗材(2) → 首页。dir = 方向(上键 -1 进耗材页时停在最后一屏)。
// 详情/耗材页底部提示常亮(v4.4 去掉了 3 次计数);首页没有提示。
static void goto_page(int page, int dir) {
    if (page == 1) {
        if (bsp_lvgl_lock(200)) { pet_ui_show_detail(true, pet_hint_on_page(1)); pet_ui_show_filament(false, false); pet_ui_filament_reset_scroll(); bsp_lvgl_unlock(); }
    } else if (page == 2) {
        status_store_get_ams(status_store_current(), &s_ams_buf);
        if (bsp_lvgl_lock(200)) {
            pet_ui_set_filament(&s_ams_buf); pet_ui_show_filament(true, pet_hint_on_page(2)); pet_ui_show_detail(false, false);
            if (dir < 0) pet_ui_filament_scroll_to_end();
            bsp_lvgl_unlock();
        }
    } else if (bsp_lvgl_lock(200)) {
        pet_ui_show_filament(false, false); pet_ui_filament_reset_scroll(); pet_ui_show_detail(false, false); bsp_lvgl_unlock();
    }
}

// 双击判定只在 >= 2 台且不在演示模式、并且在宠物页(不在设置页)时打开;否则单击立即生效
static bool dbl_enabled(void) {
    return pet_dbl_enabled(status_store_active() == SRC_DEMO, status_store_count()) && pet_ui_screen() == UI_PET && !pet_ui_settings_open();
}

static void ok_single(void) {
    bool demo = status_store_active() == SRC_DEMO;
    if (pet_ui_screen() == UI_RESET) return;
    if (pet_ui_screen() == UI_EXPIRY) { ui_screen(UI_PET, NULL); return; }   // 任意键收起
    if (pet_ui_settings_open()) {
        switch (s_set_sel) {
            case SET_DEMO: set_demo(!demo); break;
            case SET_SLEEP:                   // v4.8:关 → 2 分钟 → 5 分钟 → 关,立即保存
                s_cfg.sleep_min = sleep_min_next(s_cfg.sleep_min);
                app_config_save_sleep_min(s_cfg.sleep_min);
                ESP_LOGI(TAG, "sleep_min -> %u", (unsigned)s_cfg.sleep_min);
                break;
            case SET_FACE:                    // v4.8:全黑 → 时钟 → 线线 → 全黑,立即保存
                s_cfg.sleep_face = sleep_face_next(s_cfg.sleep_face);
                app_config_save_sleep_face(s_cfg.sleep_face);
                ESP_LOGI(TAG, "sleep_face -> %u", (unsigned)s_cfg.sleep_face);
                break;
            case SET_BACK: settings(false); header_refresh(false, 0); return;
            case SET_REPAIR: settings(false); start_portal(false); return;
            case SET_RESET:                   // v4.5.3:单击 = 打开确认页(不计时);松开后再按住 5 秒才清除
                settings(false); pet_reset_open(&s_rst, s_ok_held); ui_screen(UI_RESET, NULL); return;
            default: settings(false); header_refresh(false, 0); return;
        }
        settings_redraw();
        return;
    }
    if (pet_ui_screen() == UI_REAUTH) { start_portal(true); return; }
    if (pet_ui_screen() != UI_PET) return;
    // v4.4 演示模式:单击中键 = 下一个状态(空闲→打印中→完成→暂停→断料→失败→循环),不再自动轮换;停在当前页
    if (demo) {
        if (s_machine.shown == PET_DONE) pet_machine_ack(&s_machine);
        demo_next();
        return;
    }
    // 正常模式:首页「完成」先收下(冒爱心);否则仍按 v4.3 翻到下一页(上下键是主要翻页方式)
    if (cur_page() == 0 && s_machine.shown == PET_DONE) {
        pet_machine_ack(&s_machine);
        if (bsp_lvgl_lock(200)) { pet_ui_react(); bsp_lvgl_unlock(); }
        return;
    }
    goto_page(pet_page_step(cur_page(), 1), 1);
}

static void ok_double(void) {
    // v4.4:双击中键 = 下一台打印机(停在当前页)
    if (pet_ui_screen() == UI_PET && !pet_ui_settings_open() && status_store_count() > 1 && status_store_active() != SRC_DEMO) {
        select_printer(status_store_current() + 1, 1, true);
        if (cur_page() == 2) {                 // 耗材页换成新机器的数据
            status_store_get_ams(status_store_current(), &s_ams_buf);
            if (bsp_lvgl_lock(200)) { pet_ui_set_filament(&s_ams_buf); bsp_lvgl_unlock(); }
        }
        return;
    }
    ok_single(); ok_single();
}
static void ok_dispatch(pet_ok_ev_t e) { if (e == PET_OK_SINGLE) ok_single(); else if (e == PET_OK_DOUBLE) ok_double(); }
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void reset_cancel(void) {
    pet_reset_close(&s_rst);
    ui_screen(s_portal_on ? UI_SETUP_QR : UI_PET, setup_portal_ap_name());
    if (!s_portal_on) { settings(true); s_set_sel = SET_RESET; settings_redraw(); }
}
static void handle_key(const key_ev_t *k) {
    pet_sleep_activity(&s_slp, now_ms());   // v4.8:任何按键事件都让无操作计时清零
    if (s_asleep) return;                   // 兜底:熄屏后才处理到的旧事件不执行(守卫已拦新按键)
    bool in_set = pet_ui_settings_open();
    if (k->btn == BSP_BTN_OK) {
        switch (k->ev) {
        case BSP_BTN_PRESS:
            s_ok_held = true;
            if (pet_ui_screen() == UI_RESET) { pet_reset_press(&s_rst, now_ms()); return; }   // 确认页:只交给状态机
            ok_dispatch(pet_okkey_press(&s_ok, now_ms()));
            return;
        case BSP_BTN_RELEASE: {
            s_ok_held = false;
            if (pet_ui_screen() == UI_RESET) {
                pet_okkey_init(&s_ok);
                if (pet_reset_release(&s_rst, now_ms()) == PET_RST_EV_CANCEL) reset_cancel();   // 不到 5 秒松手 = 回设置页
                return;
            }
            pet_ok_ev_t e = pet_okkey_release(&s_ok, now_ms(), dbl_enabled());
            ok_dispatch(e);
            return;
        }
        case BSP_BTN_LONG:
            pet_okkey_long(&s_ok);
            if (pet_ui_screen() == UI_RESET) return;
            if (pet_ui_screen() == UI_SETUP_QR && s_portal_cancellable) { stop_portal_to_prev(); return; }
            if (pet_ui_screen() == UI_PET && !in_set) settings(true);
            return;
        default:
            return;   // 驱动自己的 CLICK/DOUBLE(180 ms 窗口)不用,中键由 s_ok 判定
        }
    }
    if (k->ev != BSP_BTN_CLICK) return;
    if (pet_ui_screen() == UI_RESET) { if (s_rst.st != PET_RST_HOLDING) reset_cancel(); return; }   // v4.5.3:确认页上/下 = 取消
    if (pet_ui_screen() == UI_EXPIRY) { ui_screen(UI_PET, NULL); return; }   // 任意键收起
    if (in_set) {
        s_set_sel = pet_set_step(s_set_sel, k->btn == BSP_BTN_UP ? -1 : 1, SET_COUNT);   // v4.5.3:到头停住,不回绕
        settings_redraw();
        return;
    }
    if (pet_ui_screen() != UI_PET) return;
    // v4.4:上下键只切页;耗材页一屏放不下时先在页内翻,到头/到底再切到上一页/下一页
    int dir = k->btn == BSP_BTN_DOWN ? 1 : -1;
    if (pet_ui_filament_open()) {
        bool moved = false;
        if (bsp_lvgl_lock(200)) { moved = pet_ui_filament_scroll(dir); bsp_lvgl_unlock(); }
        if (moved) return;
    }
    goto_page(pet_page_step(cur_page(), dir), dir);
}

// 每轮主循环:中键双击窗口到期补发单击;刷新底部提示文字
static void keys_tick(void) {
    ok_dispatch(pet_okkey_tick(&s_ok, now_ms()));
    bool demo = status_store_active() == SRC_DEMO;
    int n = status_store_count();
    char pages[64], fil[64];
    pet_hint_text(0, demo, n, demo ? demo_step() : 0, pages, sizeof pages);
    pet_hint_text(2, demo, n, demo ? demo_step() : 0, fil, sizeof fil);
    if (bsp_lvgl_lock(100)) {
        pet_ui_set_hints(pages, fil);   // v4.4:详情/耗材页提示常亮(不再计次),首页没有提示
        bsp_lvgl_unlock();
    }
}

static void factory_reset(void) {
    ESP_LOGW(TAG, "factory reset: erase petcfg");
    nvs_handle_t h;   // v4.5.1b:在 petboot 里留一条"第几次开机时恢复过出厂",以后开机日志能看到
    if (nvs_open("petboot", NVS_READWRITE, &h) == ESP_OK) {
        uint32_t c = 0; nvs_get_u32(h, "count", &c); nvs_set_u32(h, "freset", c); nvs_commit(h); nvs_close(h);
    }
    app_config_erase();
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
}

static void tick_reset_hold(void) {   // v4.5.3:只看状态机;倒计时只在"松开后重新按下"之后才走
    if (pet_ui_screen() != UI_RESET) { if (s_rst.st != PET_RST_OFF && s_rst.st != PET_RST_DONE) pet_reset_close(&s_rst); return; }
    uint32_t now = now_ms(), t = pet_reset_elapsed(&s_rst, now);
    int left = ((int)PET_RESET_HOLD_MS - (int)t + 999) / 1000, pct = (int)(t * 100 / PET_RESET_HOLD_MS);
    if (left < 0) left = 0;
    if (pct > 100) pct = 100;
    if (bsp_lvgl_lock(100)) { pet_ui_set_reset_progress(left, pct); bsp_lvgl_unlock(); }
    if (pet_reset_tick(&s_rst, now) == PET_RST_EV_RESET) factory_reset();
}

// 过期提醒:每天第一次检查(对时后)时,剩余 1..3 天就弹一次;已过期直接进"要重新扫码"
static void check_expiry(void) {
    if (!s_cloud_ok || app_config_lan_ready(&s_cfg) || status_store_active() != SRC_CLOUD) return;
    time_t now = time(NULL);
#if CONFIG_PET_TEST_SHORT_EXPIRY
    // 测试版:单位换成分钟;"每天一次"换成"每个 token 一次"(窗口 id = 过期时间)
    int days = pet_token_minutes_left(s_cfg.token_exp, now);
    int32_t today = (int32_t)(s_cfg.token_exp / 60);
#else
    int days = pet_token_days_left(s_cfg.token_exp, now);
    int32_t today = (int32_t)((now + 8 * 3600) / 86400);   // 北京时间日序号
#endif
    if (days < 0) return;
    if (days == 0) { if (pet_ui_screen() == UI_PET) ui_screen(UI_REAUTH, NULL); return; }
    if (pet_ui_screen() == UI_PET && pet_expiry_should_remind(days, today, s_cfg.remind_day)) {
        char d[12]; snprintf(d, sizeof d, "%d", days);
        ui_screen(UI_EXPIRY, d);
        s_cfg.remind_day = today;
        app_config_save_remind_day(today);
    }
}

// ---------------------------------------------------------------------------
// v4.8 休眠(熄屏)
// 把整屏刷新交给 taskLVGL(栈 7168)。不要在 pet_app 上调用 lv_refr_now:
// 真机日志:A 唤醒时同一路径把 stack_hw 打到 312;随后 sleep enter face=1 后 ~5 s
// task_wdt 报 pet_app 卡在 lv_draw_unit_draw_letter,没有 "sleep screen lvgl mem" 日志,
// 屏上仍是首页、背光未降 —— 首帧画在 pet_app 持锁的同步刷新里,死循环/栈破坏后没送出去。
static void sleep_yield_frame(void) {
    vTaskDelay(pdMS_TO_TICKS(80));
    bsp_display_wait_idle();
}

static void sleep_fill_info(const pet_state_t *states, int cnt, int cur) {
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    bool valid = t > 1700000000;   // 同 status_bar.c T_VALID:没对时显示 --:--
    status_bar_fmt_time(&tm, valid, s_slp_info.time, sizeof s_slp_info.time);
    s_slp_key = valid ? tm.tm_hour * 60 + tm.tm_min : -1;
    s_slp_info.soc = status_bar_soc();
    int p = pet_sleep_pick_printer(states, cnt, cur);
    s_slp_info.printing = p >= 0;
    s_slp_info.idx = p < 0 ? 0 : p;
    s_slp_info.pct = p < 0 ? 0 : s_pct_cache[p];
    s_slp_info.name[0] = s_slp_info.product[0] = 0;
    if (p >= 0) {
        status_store_get_idx(p, NULL, NULL, NULL, &s_slp_pinfo);
        snprintf(s_slp_info.name, sizeof s_slp_info.name, "%s", s_slp_pinfo.name);
        snprintf(s_slp_info.product, sizeof s_slp_info.product, "%s", s_slp_pinfo.product);
    }
    pet_sleep_shift_xy(s_slp_shift_pos, &s_slp_info.dx, &s_slp_info.dy);
}

static void sleep_enter(const pet_state_t *states, int cnt, int cur) {
    uint8_t face = sleep_face_sanitize(s_cfg.sleep_face);
    pet_sleep_actions_t a = pet_sleep_actions(face);
    if (pet_ui_settings_open()) { settings(false); header_refresh(false, 0); }   // 先关设置页,回到打开设置之前那一页
    s_slp_face = face;
    s_slp_screen = pet_ui_screen();
    s_asleep = true;                        // 先置位:从这一刻起按下的键都按「唤醒」处理
    ESP_LOGI(TAG, "sleep enter face=%u heap free=%u min=%u", (unsigned)face, (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
    if (a.panel_sleep) {
        bsp_display_backlight(a.backlight_pct);   // 80 → 0,直接关,不渐变(不调 ledc_fade_func_install)
        if (bsp_lvgl_lock(1000)) {
            bsp_display_panel_sleep(true);        // DISPOFF + SLPIN(先等完在途像素)
            pet_ui_anim_pause(true);
            pet_ui_render_pause(true);            // 暂停刷屏,对象不释放
            bsp_lvgl_unlock();
        }
        return;
    }
    s_slp_refresh_n = 0; s_slp_shift_pos = -1; s_slp_ms = now_ms();
    sleep_fill_info(states, cnt, cur);
    if (bsp_lvgl_lock(1000)) {
        // A 会 pause 刷屏;若唤醒时没 resume,带着暂停去 load B/C,失效登记仍关着,
        // 新页面不进脏区。这里成对恢复(已恢复则是空操作)。
        pet_ui_render_pause(false);
        pet_ui_sleep_enter(face, &s_slp_info);
        lv_mem_monitor_t mm; lv_mem_monitor(&mm);
        bsp_lvgl_unlock();
        ESP_LOGI(TAG, "sleep screen lvgl mem used=%u/%u max=%u", (unsigned)(mm.total_size - mm.free_size), (unsigned)mm.total_size, (unsigned)mm.max_used);
    }
    sleep_yield_frame();                          // taskLVGL 画完再降背光,避免首页全亮残帧
    if (bsp_lvgl_lock(200)) { pet_ui_sleep_reap(); bsp_lvgl_unlock(); }
    bsp_display_backlight(a.backlight_pct);       // 80 → 8%(同一套 LEDC,频率不变)
}

static void sleep_wake(const char *why) {
    if (!s_asleep) return;
    ESP_LOGI(TAG, "sleep wake begin (%s) face=%u", why ? why : "?", (unsigned)s_slp_face);
    if (s_slp_face == SLEEP_FACE_BLACK) {
        if (bsp_lvgl_lock(1000)) {
            bsp_display_panel_sleep(false);       // SLPOUT(>= 120 ms 间隔 + 驱动内 100 ms)
            pet_ui_render_pause(false);           // 恢复刷屏并整屏失效
            pet_ui_anim_pause(false);
            bsp_lvgl_unlock();
        }
        sleep_yield_frame();                      // 交给 taskLVGL 画休眠前那一页
        if (bsp_lvgl_lock(1000)) {
            bsp_display_on(true);                 // DISPON:先等像素传完
            bsp_lvgl_unlock();
        }
    } else if (bsp_lvgl_lock(1000)) {
        pet_ui_sleep_exit();
        bsp_lvgl_unlock();
        sleep_yield_frame();                      // 先让 taskLVGL 画出首页
        if (bsp_lvgl_lock(1000)) { pet_ui_sleep_reap(); bsp_lvgl_unlock(); }
    }
    bsp_display_backlight(SLEEP_BL_AWAKE_PCT);   // 画面到屏以后才开背光,不会先亮出旧画面
    s_asleep = false;
    pet_okkey_init(&s_ok); s_ok_held = false;    // 清掉熄屏前可能残留的中键状态
    pet_sleep_set_awake(&s_slp, now_ms());
    ESP_LOGI(TAG, "sleep wake (%s) heap free=%u min=%u", why, (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
}

// 每轮主循环调用一次(在 states[] 算完之后)
static void sleep_tick(const pet_state_t *states, int cnt, int cur) {
    uint32_t now = now_ms();
    bool demo = status_store_active() == SRC_DEMO;
    // 自动亮屏:只看「变成」完成/失败/暂停/断料;演示模式不跟踪
    int pause_idx = -1, hits = 0;
    if (demo) pet_wake_reset(s_wake_prev);
    else {
        pet_state_t eff[PET_MAX_PRINTERS];   // 没有有效报文的机器记成「没见过」:重连后的第一个状态不算变化
        for (int i = 0; i < cnt && i < PET_MAX_PRINTERS; i++) eff[i] = s_valid_cache[i] ? states[i] : PET_STATE_COUNT;
        hits = pet_wake_check(s_wake_prev, eff, cnt, &pause_idx);
    }
    if (s_wake_req) { s_wake_req = false; sleep_wake("key"); return; }
    if (hits > 0) {
        if (s_asleep) {
            if (pause_idx >= 0 && pet_ui_screen() == UI_PET && !pet_ui_settings_open())
                select_printer(pause_idx, pause_idx > cur ? 1 : -1, false);   // 暂停不在现有自动跳转里,这里切过去
            sleep_wake("print state");
        } else pet_sleep_activity(&s_slp, now);   // 亮着时也重新计时,让人看到
        return;
    }
    bool allowed = pet_sleep_allowed(pet_ui_screen(), s_portal_on, demo, s_cfg.sleep_min);
    if (s_asleep) {
        if (!allowed || pet_ui_screen() != s_slp_screen) { sleep_wake("screen"); return; }   // 进了重新登录/到期提醒等
        if (s_slp_face != SLEEP_FACE_BLACK) {
            time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
            int key = t > 1700000000 ? tm.tm_hour * 60 + tm.tm_min : -1;
            bool due = pet_sleep_refresh_due(s_slp_key, key, s_slp_ms, now);
            bool was_printing = s_slp_info.printing;
            int was_idx = s_slp_info.idx, was_pct = s_slp_info.pct;
            static char was_name[sizeof s_slp_info.name], was_product[sizeof s_slp_info.product];
            snprintf(was_name, sizeof was_name, "%s", s_slp_info.name);
            snprintf(was_product, sizeof was_product, "%s", s_slp_info.product);
            sleep_fill_info(states, cnt, cur);
            bool info_changed = pet_sleep_content_changed(was_printing, was_idx, was_pct, was_name, was_product,
                                                          s_slp_info.printing, s_slp_info.idx, s_slp_info.pct,
                                                          s_slp_info.name, s_slp_info.product);
            if (due) {
                s_slp_ms = now; s_slp_refresh_n++;
                if (pet_sleep_shift_due(key, s_slp_refresh_n)) s_slp_shift_pos = (s_slp_shift_pos + 1) % 4;
                pet_sleep_shift_xy(s_slp_shift_pos, &s_slp_info.dx, &s_slp_info.dy);
            }
            if ((due || info_changed) && bsp_lvgl_lock(200)) { pet_ui_sleep_update(&s_slp_info); bsp_lvgl_unlock(); }
        }
        return;
    }
    if (pet_sleep_tick(&s_slp, now, allowed, s_cfg.sleep_min) == PET_SLEEP_EV_ENTER) sleep_enter(states, cnt, cur);
}

#if CONFIG_PET_DEBUG_STATUS_INJECT || CONFIG_PET_TEST_SHORT_EXPIRY
// 仅调试/测试构建:往当前状态源写入假的 job/改名/清空,供 B/C 熄屏刷新真机过一遍。发布包不编译。
static void inject_job(void) {
    printer_status_t st;
    status_store_get_idx(0, &st, NULL, NULL, NULL);
    st.valid = true;
    if (st.n_nozzles <= 0) { st.n_nozzles = 1; st.noz[0] = 220.0f; st.noz_t[0] = 220.0f; }
    snprintf(st.gcode_state, sizeof st.gcode_state, "RUNNING");
    static unsigned seq;
    snprintf(st.job_id, sizeof st.job_id, "inj%u", ++seq);
    snprintf(st.subtask, sizeof st.subtask, "inject_job");
    st.percent = 20 + (int)(seq * 17 % 60);
    status_store_set_idx(status_store_active(), 0, &st);
    ESP_LOGW(TAG, "inject job id=%s pct=%d (asleep=%d)", st.job_id, st.percent, (int)s_asleep);
}
static void inject_rename(void) {
    int n = status_store_count();
    if (n < 1) n = 1;
    printer_info_t info[PET_MAX_PRINTERS];
    for (int i = 0; i < n && i < PET_MAX_PRINTERS; i++)
        status_store_get_idx(i, NULL, NULL, NULL, &info[i]);
    static unsigned seq;
    snprintf(info[0].name, sizeof info[0].name, "LongName-%02u-ABCDEFGHIJKLMNOPQRSTUVWXYZ", ++seq);
    if (!info[0].product[0]) snprintf(info[0].product, sizeof info[0].product, "H2C");
    status_store_set_info(status_store_active(), n, info);
    ESP_LOGW(TAG, "inject rename -> %s (asleep=%d)", info[0].name, (int)s_asleep);
}
static void inject_clear(void) {
    printer_status_t st;
    status_store_get_idx(0, &st, NULL, NULL, NULL);
    st.valid = true;
    snprintf(st.gcode_state, sizeof st.gcode_state, "IDLE");
    st.job_id[0] = 0;
    st.subtask[0] = 0;
    st.percent = 0;
    status_store_set_idx(status_store_active(), 0, &st);
    ESP_LOGW(TAG, "inject clear (asleep=%d)", (int)s_asleep);
}
#if CONFIG_PET_DEBUG_STATUS_INJECT && !CONFIG_PET_TEST_SHORT_EXPIRY
static volatile bool s_cap;
static volatile int s_cap_rows;
static char s_cap_tag[16];
static unsigned char s_cap_b64[244 * 2 * 4 / 3 + 8];
static bool s_cap_row_seen[320];
static void inject_flush_ev(lv_event_t *e) {
    if (!s_cap) return;
    lv_display_t *d = lv_event_get_target(e);
    const lv_area_t *a = lv_event_get_param(e);
    lv_draw_buf_t *b = lv_display_get_buf_active(d);
    if (!a || !b || !b->data) return;
    int w = a->x2 - a->x1 + 1;
    if (w <= 0) return;
    for (int y = a->y1; y <= a->y2; y++) {
        const uint8_t *row = b->data + (y - a->y1) * b->header.stride;
        size_t ol = 0;
        if (mbedtls_base64_encode(s_cap_b64, sizeof s_cap_b64, &ol, row, (size_t)w * 2) != 0) continue;
        s_cap_b64[ol] = 0;
        printf("@FB %d %d %d %s\n", y, (int)a->x1, (int)a->x2, s_cap_b64);
        if (y >= 0 && y < 320 && !s_cap_row_seen[y]) { s_cap_row_seen[y] = true; s_cap_rows++; }
    }
    if (s_cap_rows >= lv_display_get_vertical_resolution(d)) {
        s_cap = false;
        printf("@FBEND %s %d\n", s_cap_tag, s_cap_rows);
        fflush(stdout);
    }
}
static void inject_cap_start(const char *tag) {
    lv_display_t *d = lv_display_get_default();
    if (!d || s_cap) { ESP_LOGW(TAG, "inject cap skip (d=%p busy=%d)", (void *)d, (int)s_cap); return; }
    snprintf(s_cap_tag, sizeof s_cap_tag, "%s", tag && tag[0] ? tag : "screen");
    memset(s_cap_row_seen, 0, sizeof s_cap_row_seen); s_cap_rows = 0;
    printf("@FBBEGIN %s %d %d\n", s_cap_tag,
           (int)lv_display_get_horizontal_resolution(d),
           (int)lv_display_get_vertical_resolution(d));
    s_cap = true;
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
}
#endif
static bool handle_inject_cmd(const char *cmd) {
    if (!cmd || !cmd[0]) return false;
    if (!strcmp(cmd, "injjob")) { inject_job(); return true; }
    if (!strcmp(cmd, "injren")) { inject_rename(); return true; }
    if (!strcmp(cmd, "injclr")) { inject_clear(); return true; }
    if (!strcmp(cmd, "injwake")) { s_wake_req = true; ESP_LOGW(TAG, "inject wake"); return true; }
    if (!strcmp(cmd, "injsleep")) {
        if (s_asleep) { ESP_LOGW(TAG, "inject sleep ignored (already asleep)"); return true; }
        if (sleep_min_sanitize(s_cfg.sleep_min) == SLEEP_MIN_OFF) {
            ESP_LOGW(TAG, "inject sleep ignored (sleep_min off)"); return true;
        }
        s_slp.idle_since_ms = now_ms() - (uint32_t)sleep_min_sanitize(s_cfg.sleep_min) * 60000u;
        ESP_LOGW(TAG, "inject sleep (idle backdated)");
        return true;
    }
    if (!strncmp(cmd, "injface ", 8)) {
        s_cfg.sleep_face = sleep_face_sanitize((uint8_t)atoi(cmd + 8));
        ESP_LOGW(TAG, "inject sleep_face -> %u", (unsigned)s_cfg.sleep_face);
        return true;
    }
    if (!strcmp(cmd, "injmem")) {
        if (bsp_lvgl_lock(200)) {
            lv_mem_monitor_t mm; lv_mem_monitor(&mm);
            bsp_lvgl_unlock();
            ESP_LOGI(TAG, "injmem lvgl used=%u/%u free=%u max=%u frag=%u%%",
                     (unsigned)(mm.total_size - mm.free_size), (unsigned)mm.total_size,
                     (unsigned)mm.free_size, (unsigned)mm.max_used, (unsigned)mm.frag_pct);
        }
        return true;
    }
    if (!strncmp(cmd, "injpg ", 6)) {
        if (s_asleep) { ESP_LOGW(TAG, "inject page ignored (asleep)"); return true; }
        int p = atoi(cmd + 6);
        if (pet_ui_settings_open()) settings(false);
        goto_page(p, 1);
        ESP_LOGW(TAG, "inject page -> %d", p);
        return true;
    }
    if (!strcmp(cmd, "injset")) {
        if (s_asleep) { ESP_LOGW(TAG, "inject settings ignored (asleep)"); return true; }
        if (s_portal_on) { ESP_LOGW(TAG, "inject settings ignored (portal)"); return true; }
        settings(true);
        ESP_LOGW(TAG, "inject settings");
        return true;
    }
    if (!strcmp(cmd, "injqr")) {
        if (s_asleep) { ESP_LOGW(TAG, "inject qr ignored (asleep)"); return true; }
        settings(false);
        start_portal(false);
        ESP_LOGW(TAG, "inject qr hint=%d", (int)s_portal_cancellable);
        return true;
    }
    if (!strcmp(cmd, "injqroff")) {
        if (s_portal_on) stop_portal_to_prev();
        ESP_LOGW(TAG, "inject qr off");
        return true;
    }
#if CONFIG_PET_DEBUG_STATUS_INJECT && !CONFIG_PET_TEST_SHORT_EXPIRY
    if (!strcmp(cmd, "injcap") || !strncmp(cmd, "injcap ", 7)) {
        const char *tag = (cmd[6] == ' ') ? cmd + 7 : "screen";
        if (bsp_lvgl_lock(500)) { inject_cap_start(tag); bsp_lvgl_unlock(); }
        ESP_LOGW(TAG, "inject cap %s", tag);
        return true;
    }
#endif
    return false;
}
#if CONFIG_PET_DEBUG_STATUS_INJECT && !CONFIG_PET_TEST_SHORT_EXPIRY
static void poll_inject_stdin(void) {
    // USB-Serial-JTAG 控制台不会把主机写入交到 stdin;与测试版 devcap 一样走驱动读字节。
    // 不另起任务:在 pet_app 里非阻塞读。rx/tx 各 256 B,大约多占 0.5 KB 堆。
    static char buf[32];
    static int n;
    static bool inited;
    if (!inited) {
        usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        cfg.rx_buffer_size = 256;
        cfg.tx_buffer_size = 1024;   // 一帧行 base64 ~640 B
        if (usb_serial_jtag_driver_install(&cfg) == ESP_OK) usb_serial_jtag_vfs_use_driver();
        if (bsp_lvgl_lock(200)) {
            lv_display_t *d = lv_display_get_default();
            if (d) lv_display_add_event_cb(d, inject_flush_ev, LV_EVENT_FLUSH_START, NULL);
            bsp_lvgl_unlock();
        }
        inited = true;
    }
    uint8_t c;
    while (usb_serial_jtag_read_bytes(&c, 1, 0) > 0) {
        if (c == '\r' || c == '\n') {
            if (n) { buf[n] = 0; n = 0; handle_inject_cmd(buf); }
        } else if (n < (int)sizeof buf - 1) buf[n++] = (char)c;
    }
}
#endif
#endif

static void app_task(void *arg) {
    (void)arg;
    uint32_t n = 0;
    job_load();
    pet_switch_init(&s_switch, status_store_count());
    pet_sleep_init(&s_slp, now_ms());
    pet_wake_reset(s_wake_prev);
    int last_count = -1;
    bool last_online = false;
    for (;;) {
        key_ev_t k;
        while (xQueueReceive(s_keys, &k, 0) == pdTRUE) handle_key(&k);
        tick_reset_hold();
        keys_tick();
        int cnt = status_store_count();
        if (cnt != last_count) { last_count = cnt; pet_switch_init(&s_switch, cnt); s_switch.cur = status_store_current(); header_refresh(false, 0); pet_wake_reset(s_wake_prev); }
        // 所有机器:更新锁存状态机 + 用时记录;前台显示当前那台
        static pet_state_t states[PET_MAX_PRINTERS];
        static printer_status_t st, cur_st;      // 静态:不占 pet_app 栈(这两份以前压在栈上,再叠加同步刷屏会把水位打到几百)
        link_state_t link = LINK_UNCONFIGURED;
        printer_status_reset(&cur_st);
        int cur = status_store_current();
        bool online = false;
        for (int i = 0; i < cnt; i++) {
            bool on;
            status_store_get_idx(i, &st, &link, &on, NULL);
            if (link != LINK_DEMO) {   // 演示数据自带 elapsed_min
                if (pet_job_update(&s_jobs[i], &st, (int64_t)time(NULL))) {
                    job_save(i);
                    s_jobs[i].job[sizeof s_jobs[i].job - 1] = 0;
                    ESP_LOGI(TAG, "#%d job %s start=%lld end=%lld%s", i, s_jobs[i].job[0] ? s_jobs[i].job : "?",
                             (long long)s_jobs[i].start, (long long)s_jobs[i].end, s_asleep ? " (asleep)" : "");
                }
                st.elapsed_min = pet_job_elapsed_min(&s_jobs[i], &st);
            }
            states[i] = pet_machine_update(&s_machines[i], &st);
            s_pct_cache[i] = st.percent;
            s_valid_cache[i] = st.valid;
            if (i == cur) { cur_st = st; online = on; }
        }
        // 云端鉴权失败(静默刷新也失败)才进入"要重新扫码"
        if (link == LINK_AUTH_FAIL && pet_ui_screen() == UI_PET && !s_portal_on) ui_screen(UI_REAUTH, NULL);
        if (s_portal_on && setup_portal_state() == PORTAL_BOUND && pet_ui_screen() != UI_BOUND)
            ui_screen(UI_BOUND, setup_portal_bound_name());
        // 自动跳转:断料/失败/完成停 3 秒后跳过去(设置页/配网页打开时不跳)。
        // v4.2.1:演示模式里完全不自动跳转(演示只演示第一台的状态,跳走会打断演示);退出演示时 set_demo 会重置 s_switch。
        sleep_tick(states, cnt, cur);   // v4.8 休眠:计时、自动亮屏、B/C 每 60 s 刷新
        int jump = (status_store_active() == SRC_DEMO) ? -1
                 : pet_switch_tick(&s_switch, states, (uint32_t)(esp_timer_get_time() / 1000));
        if (jump >= 0 && !s_asleep && pet_ui_screen() == UI_PET && !pet_ui_settings_open()) {
            select_printer(jump, jump > cur ? 1 : -1, false);
            continue;
        }
        if (!s_asleep && (online != last_online || (n % 100) == 0)) { last_online = online; header_refresh(false, 0); }   // 绿点 / 改名
        else last_online = online;
        // v4.4.1:收到第一条带温度的报文才算有数据;之前前 30 秒「连接中」,之后「未连上」
        bool has_data = status_store_active() == SRC_DEMO || (online && cur_st.valid && cur_st.n_nozzles > 0);
        bool mqtt_up = status_store_active() == SRC_CLOUD && link == LINK_ONLINE;   // v4.4.3:MQTT 已连上
        pet_conn_state_t cs = pet_conn_update_ex(&s_conn, has_data, mqtt_up, mqtt_up && bambu_cloud_parse_failing(),
                                                 (uint32_t)(esp_timer_get_time() / 1000));
        static pet_conn_state_t last_cs = PET_CONN_DATA;
        if (cs != last_cs) { ESP_LOGI(TAG, "conn -> %s", cs == PET_CONN_DATA ? "data" : cs == PET_CONN_CONNECTING ? "connecting" : cs == PET_CONN_NODATA ? "nodata (mqtt up, no report)" : "failed"); last_cs = cs; }
        // 熄屏时不要再改首页/详情对象:FINISH 等状态变化会走 start_transition/apply_state,
        // 真机 B 段曾在 "#0 job … end=…" 之后 Load access fault。亮屏(含自动唤醒)之后 s_asleep 已清,这里照常刷新。
        if (!s_asleep && bsp_lvgl_lock(100)) {
            pet_ui_set_conn(cs);
            pet_ui_set_demo_step(status_store_active() == SRC_DEMO ? demo_step() : -1);
            pet_ui_update(states[cur], &cur_st, link);
            bsp_lvgl_unlock();
        }
        if (!s_asleep && pet_ui_filament_open()) {                // v4.3:耗材页开着时每轮刷新(数据没变 UI 不重画)
            status_store_get_ams(cur, &s_ams_buf);
            if (bsp_lvgl_lock(100)) { pet_ui_set_filament(&s_ams_buf); pet_ui_filament_set_spinning(states[cur] == PET_PRINT); bsp_lvgl_unlock(); }
        }
        {   // 仅测试版:串口命令(正式版 devcap_poll_cmd 恒为 false)
            char cmd[32];
            if (devcap_poll_cmd(cmd, sizeof cmd)) {
                ESP_LOGW(TAG, "serial cmd: %s", cmd);
                if (!strncmp(cmd, "cap", 3)) { if (bsp_lvgl_lock(500)) { devcap_capture(cmd[3] == ' ' ? cmd + 4 : "screen"); bsp_lvgl_unlock(); } }
                else if (!strcmp(cmd, "demo1")) { if (status_store_active() != SRC_DEMO) set_demo(true); }
                else if (!strcmp(cmd, "demo0")) { if (status_store_active() == SRC_DEMO) set_demo(false); }
                else if (!strncmp(cmd, "demostep ", 9)) {   // 1..6;和单击中键同一路径(demo_next),只在演示模式
                    int want = atoi(cmd + 9) - 1;
                    if (status_store_active() == SRC_DEMO && want >= 0 && want < 6)
                        for (int i = 0; i < 6 && demo_step() != want; i++) demo_next();
                }
                else if (!strcmp(cmd, "portal")) { settings(false); start_portal(false); }   // 只开热点+页面,不写 NVS
#if CONFIG_PET_DEBUG_STATUS_INJECT || CONFIG_PET_TEST_SHORT_EXPIRY
                else if (handle_inject_cmd(cmd)) { /* injjob / injren / injclr / injwake / injface N */ }
#endif
                else if (!strcmp(cmd, "reboot")) esp_restart();
            }
        }
#if CONFIG_PET_DEBUG_STATUS_INJECT && !CONFIG_PET_TEST_SHORT_EXPIRY
        poll_inject_stdin();
#endif
        status_bar_poll();                  // v4.7:电量(内部 30 s 限频,I2C 不在 LVGL 线程)
        if (n % 200 == 0) check_expiry();   // 约每 10 秒
        if (++n % 600 == 0) {
            ESP_LOGI(TAG, "heap free=%u min=%u largest=%u printers=%d", (unsigned)esp_get_free_heap_size(),
                     (unsigned)esp_get_minimum_free_heap_size(),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), cnt);
            if (bsp_lvgl_lock(100)) {
                lv_mem_monitor_t mm; lv_mem_monitor(&mm);
                bsp_lvgl_unlock();
                ESP_LOGI(TAG, "lvgl mem used=%u/%u max=%u frag=%u%%", (unsigned)(mm.total_size - mm.free_size), (unsigned)mm.total_size,
                         (unsigned)mm.max_used, (unsigned)mm.frag_pct);
            }
#if CONFIG_PET_LOG_REPORT_KEYS && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
            {   // v4.4.1 测试版:各任务累计 CPU 占比(看 taskLVGL / bambu_tok / IDLE 有没有拿到 CPU)
                char *rt = malloc(1024);
                if (rt) {
                    vTaskGetRunTimeStats(rt);
                    for (char *ln = strtok(rt, "\r\n"); ln; ln = strtok(NULL, "\r\n")) ESP_LOGI(TAG, "cpu %s", ln);
                    free(rt);
                }
            }
#endif
#if CONFIG_PET_LOG_REPORT_KEYS
            ESP_LOGI(TAG, "conn #%d: online=%d valid=%d nozzles=%d", cur, online, cur_st.valid, cur_st.n_nozzles);
            char tn[2][20], tb[20], tc[20];
            pet_format_temp(cur_st.noz[0], cur_st.noz_t[0], tn[0], sizeof tn[0]); pet_format_temp(cur_st.noz[1], cur_st.noz_t[1], tn[1], sizeof tn[1]);
            pet_format_temp(cur_st.bed, cur_st.bed_target, tb, sizeof tb); pet_format_temp(cur_st.chamber, cur_st.chamber_target, tc, sizeof tc);
            ESP_LOGI(TAG, "detail #%d: task=\"%s\" %s %d%% layer %d/%d rem=%d nozzles=%d active=%d L=%s R=%s bed=%s chamber=%s(%d) fil=%s/%06lX %s hum=%d",
                     cur, cur_st.subtask, cur_st.gcode_state, cur_st.percent, cur_st.layer, cur_st.total_layer, cur_st.remaining_min,
                     cur_st.n_nozzles, cur_st.active_nozzle, tn[0], tn[1], tb, tc, cur_st.has_chamber, cur_st.fil_type,
                     (unsigned long)cur_st.fil_rgb, cur_st.fil_slot, cur_st.fil_humidity);
            // v4.3 耗材页核对:每台 AMS 一行(和耗材页同一份数据)
            status_store_get_ams(cur, &s_ams_buf);
            for (int u = 0; u < s_ams_buf.n_units; u++) {
                const pet_ams_unit_t *au = &s_ams_buf.u[u];
                char nm[8], hm[8], row[160]; int o = 0;
                pet_ams_unit_name(au->id, nm, sizeof nm); pet_ams_humidity_text(au, hm, sizeof hm);
                for (int sl = 0; sl < au->nslot && o < (int)sizeof row - 40; sl++) {
                    const pet_tray_t *t = &s_ams_buf.slot[au->first + sl];
                    if (t->flags & PET_TRAY_EMPTY) o += snprintf(row + o, sizeof row - o, " [%d 空]", sl + 1);
                    else o += snprintf(row + o, sizeof row - o, " [%d %s %02X%02X%02X r=%d%s%s]", sl + 1, t->type, t->r, t->g, t->b,
                                       t->remain, pet_tray_low(t) ? " low" : "", (t->flags & PET_TRAY_ACTIVE) ? " *" : "");
                }
                ESP_LOGI(TAG, "ams #%d %s id=%d hum=%s(raw %d lvl %d):%s", cur, nm, au->id, hm, au->hum_pct, au->hum_lvl, row);
            }
            for (int e = 0; e < s_ams_buf.n_ext; e++) {
                const pet_tray_t *t = &s_ams_buf.ext[e];
                ESP_LOGI(TAG, "ams #%d ext %s: %s %02X%02X%02X%s", cur, (t->flags & PET_TRAY_SIDE_L) ? "L" : (t->flags & PET_TRAY_SIDE_R) ? "R" : "-",
                         (t->flags & PET_TRAY_EMPTY) ? "unset" : t->type, t->r, t->g, t->b, (t->flags & PET_TRAY_ACTIVE) ? " *" : "");
            }
            if (!s_ams_buf.valid) ESP_LOGI(TAG, "ams #%d: no ams data yet", cur);
#endif
        }
        if (xQueueReceive(s_keys, &k, pdMS_TO_TICKS(50)) == pdTRUE) handle_key(&k);
    }
}

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 不可用 (%s),不擦除;配置走编译期默认值", esp_err_to_name(err));
    } else if (err != ESP_OK) ESP_LOGW(TAG, "nvs_flash_init = %s", esp_err_to_name(err));
    nvs_stats_t ns;   // v4.5.1b:开机打一行 NVS 占用,方便判断满了/被擦了
    if (nvs_get_stats(NULL, &ns) == ESP_OK)
        ESP_LOGI(TAG, "nvs used=%u free=%u total=%u namespaces=%u", (unsigned)ns.used_entries, (unsigned)ns.free_entries,
                 (unsigned)ns.total_entries, (unsigned)ns.namespace_count);
    log_boot_reason();
    bsp_i2c_init();
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示初始化失败");
        return;
    }
    bsp_display_backlight(80);
    status_store_init();
    for (int i = 0; i < PET_MAX_PRINTERS; i++) pet_machine_init(&s_machines[i]);
    if (bsp_lvgl_lock(1000)) { pet_ui_create(); status_bar_create(); bsp_lvgl_unlock(); }   // v4.7:顶部时间+电池
    devcap_init();   // 仅测试版:串口命令/抓屏(正式版空函数)

    s_keys = xQueueCreate(8, sizeof(key_ev_t));
    s_input_ready = s_keys != NULL;
    if (bsp_button_init(on_key, NULL) != ESP_OK) ESP_LOGE(TAG, "按键初始化失败");

    app_config_load(&s_cfg);
    s_cloud_ok = bambu_cloud_start(&s_cfg);   // 已绑定:自动连 Wi-Fi + 云端,断线指数退避重连
    if (s_cloud_ok) {
        status_store_activate(SRC_CLOUD);
        ESP_LOGI(TAG, "云端模式");
    } else {
        status_store_activate(SRC_DEMO);       // 配网页背后用演示数据,长按下可切到演示
        start_portal(false);
        ESP_LOGI(TAG, "未绑定,进入配网");
    }
    xTaskCreate(app_task, "pet_app", 5120, NULL, 4, NULL);   // v4.8 hotfix:刷屏已交回 taskLVGL(7168),不再在 pet_app 上 lv_refr_now。栈保持 5120(曾试 6144,真机 B 段 min_heap 40784 低于 40 KB,已调回)。
}
