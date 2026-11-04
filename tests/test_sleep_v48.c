// v4.8 休眠(熄屏)纯逻辑:超时 2/5/关、不休眠的页面、设置页先关再睡、唤醒回原页、三种熄屏画面的动作
// (只有 A 发 SLPIN)、60 s 刷新节拍与防残影、B 显示哪台及版式、自动亮屏(变化 vs 第一次看到、4 种状态、暂停切机)、
// 设置页 6 行布局和序号→动作映射、设置文案。纯 C。
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "sleep_logic.h"

int main(void) {
    // ---- 允许休眠的页面 ----
    assert(pet_sleep_allowed(UI_PET, false, false, 2));
    assert(pet_sleep_allowed(UI_PET, false, false, 5));
    assert(pet_sleep_allowed(UI_EXPIRY, false, false, 2));
    assert(!pet_sleep_allowed(UI_PET, false, false, 0));           // 关 = 永不休眠
    assert(!pet_sleep_allowed(UI_PET, true, false, 2));            // 配网/热点开着
    assert(!pet_sleep_allowed(UI_PET, false, true, 2));            // 演示模式
    assert(!pet_sleep_allowed(UI_SETUP_QR, false, false, 2));      // 二维码/热点页
    assert(!pet_sleep_allowed(UI_BOUND, false, false, 2));
    assert(!pet_sleep_allowed(UI_REAUTH, false, false, 2));        // 重新登录
    assert(!pet_sleep_allowed(UI_RESET, false, false, 2));         // 恢复出厂确认
    assert(pet_sleep_allowed(UI_PET, false, false, 7));            // 非法值按 2 分钟
    puts("sleep allowed pages: PASS");

    // ---- 超时 2 / 5 / 关 ----
    pet_sleep_t s;
    pet_sleep_init(&s, 1000);
    assert(pet_sleep_tick(&s, 1000 + 119999, true, 2) == PET_SLEEP_EV_NONE);
    assert(pet_sleep_tick(&s, 1000 + 120000, true, 2) == PET_SLEEP_EV_ENTER && s.asleep);
    assert(pet_sleep_tick(&s, 1000 + 999999, true, 2) == PET_SLEEP_EV_NONE);    // 已睡:不重复进入
    pet_sleep_set_awake(&s, 200000); assert(!s.asleep);
    assert(pet_sleep_tick(&s, 200000 + 299999, true, 5) == PET_SLEEP_EV_NONE);
    assert(pet_sleep_tick(&s, 200000 + 300000, true, 5) == PET_SLEEP_EV_ENTER);
    pet_sleep_init(&s, 0);
    for (uint32_t t = 0; t < 3600000u; t += 10000) assert(pet_sleep_tick(&s, t, true, 0) == PET_SLEEP_EV_NONE);   // 关:一小时也不睡
    // 按键清零
    pet_sleep_init(&s, 0);
    assert(pet_sleep_tick(&s, 100000, true, 2) == PET_SLEEP_EV_NONE);
    pet_sleep_activity(&s, 100000);
    assert(pet_sleep_tick(&s, 219999, true, 2) == PET_SLEEP_EV_NONE);
    assert(pet_sleep_tick(&s, 220000, true, 2) == PET_SLEEP_EV_ENTER);
    // 不允许的页面上计时暂停,离开后从头计
    pet_sleep_init(&s, 0);
    assert(pet_sleep_tick(&s, 110000, true, 2) == PET_SLEEP_EV_NONE);
    assert(pet_sleep_tick(&s, 500000, false, 2) == PET_SLEEP_EV_NONE);         // 在配网页待了很久
    assert(pet_sleep_tick(&s, 500001, true, 2) == PET_SLEEP_EV_NONE);          // 回来不会立刻睡
    assert(pet_sleep_tick(&s, 620000, true, 2) == PET_SLEEP_EV_ENTER);
    // uint32 毫秒回绕(约 49.7 天)
    pet_sleep_init(&s, 0xFFFFFF00u);
    assert(pet_sleep_tick(&s, 0xFFFFFF00u + 120000u, true, 2) == PET_SLEEP_EV_ENTER);
    puts("sleep timeout 2/5/off + key reset + paused pages: PASS");

    // ---- 设置页先关再睡;唤醒回到原页 ----
    for (int page = 0; page < 3; page++) {
        pet_view_t v = { page, true };
        pet_view_t e = pet_sleep_view_on_enter(v);
        assert(!e.settings && e.page == page);   // 设置页关掉,回到打开设置之前那一页;唤醒显示的就是它
        pet_view_t v2 = { page, false };
        e = pet_sleep_view_on_enter(v2);
        assert(!e.settings && e.page == page);   // 首页/详情/耗材原样
    }
    puts("settings closed before sleep, wake returns page: PASS");

    // ---- 三种熄屏画面的动作:只有 A 发 SLPIN ----
    pet_sleep_actions_t a = pet_sleep_actions(SLEEP_FACE_BLACK);
    assert(a.panel_sleep && a.pause_render && !a.sleep_screen && !a.hide_status_bar && a.backlight_pct == 0 && a.stop_pet_anim);
    a = pet_sleep_actions(SLEEP_FACE_CLOCK);
    assert(!a.panel_sleep && !a.pause_render && a.sleep_screen && a.hide_status_bar && a.backlight_pct == SLEEP_BL_DIM_PCT);
    a = pet_sleep_actions(SLEEP_FACE_PET);
    assert(!a.panel_sleep && !a.pause_render && a.sleep_screen && a.hide_status_bar && a.backlight_pct == SLEEP_BL_DIM_PCT && a.stop_pet_anim);
    a = pet_sleep_actions(9);                                  // 非法 → A
    assert(a.panel_sleep && a.backlight_pct == 0);
    assert(SLEEP_BL_DIM_PCT >= 5 && SLEEP_BL_DIM_PCT <= 10 && SLEEP_BL_AWAKE_PCT == 80);
    assert((1023u * SLEEP_BL_DIM_PCT) / 100u == 81);           // 同一套 10 位 duty(bsp_display_backlight 的换算)
    puts("face actions (SLPIN only for A): PASS");

    // ---- 60 s 刷新节拍 ----
    assert(!pet_sleep_refresh_due(14 * 60 + 32, 14 * 60 + 32, 0, 59000));
    assert(pet_sleep_refresh_due(14 * 60 + 32, 14 * 60 + 33, 0, 20000));      // 分钟变了:刷
    assert(!pet_sleep_refresh_due(-1, -1, 0, 59999));                         // 没对时:按 60 s
    assert(pet_sleep_refresh_due(-1, -1, 0, 60000));
    assert(pet_sleep_refresh_due(-1, 600, 0, 1));                             // 刚对上时:立刻刷
    assert(pet_sleep_refresh_due(23 * 60 + 59, 0, 0, 1));                     // 跨零点
    // 一小时里按分钟变化刷新,正好 60 次
    int n = 0, last = 10 * 60; for (uint32_t t = 0; t < 3600000u; t += 1000) { int key = 10 * 60 + (int)(t / 60000u) % 60 + ((t / 60000u) >= 60 ? 60 : 0); if (pet_sleep_refresh_due(last, key, 0, t)) { n++; last = key; } }
    assert(n == 59);   // 10:00 进入时已经画过,之后 10:01..10:59 = 59 次
    // 防残影
    assert(pet_sleep_shift_due(14 * 60 + 35, 1) && !pet_sleep_shift_due(14 * 60 + 36, 5));
    assert(!pet_sleep_shift_due(-1, 4) && pet_sleep_shift_due(-1, 5) && pet_sleep_shift_due(-1, 10) && !pet_sleep_shift_due(-1, 0));
    int dx, dy;
    pet_sleep_shift_xy(-1, &dx, &dy); assert(dx == 0 && dy == 0);
    pet_sleep_shift_xy(0, &dx, &dy); assert(dx == -3 && dy == -3);
    pet_sleep_shift_xy(1, &dx, &dy); assert(dx == 3 && dy == -3);
    pet_sleep_shift_xy(2, &dx, &dy); assert(dx == 3 && dy == 3);
    pet_sleep_shift_xy(3, &dx, &dy); assert(dx == -3 && dy == 3);
    pet_sleep_shift_xy(4, &dx, &dy); assert(dx == -3 && dy == -3);
    puts("60 s refresh tick + anti burn-in shift: PASS");

    // ---- B 显示哪台 + 版式 ----
    pet_state_t st[PET_MAX_PRINTERS] = { PET_IDLE, PET_PRINT, PET_PRINT, PET_IDLE };
    assert(pet_sleep_pick_printer(st, 4, 2) == 2);            // 当前那台在打印:显示它
    assert(pet_sleep_pick_printer(st, 4, 0) == 1);            // 否则第一台在打印的
    assert(pet_sleep_pick_printer(st, 1, 0) == -1);           // 只看前 n 台
    pet_state_t idle[PET_MAX_PRINTERS] = { PET_IDLE, PET_DONE, PET_PAUSE, PET_FAIL };
    assert(pet_sleep_pick_printer(idle, 4, 0) == -1);         // 没在打印:只显示时间 + 电量
    int tcy, bcy;
    pet_sleep_b_layout(true, &tcy, &bcy); assert(tcy == 118 && bcy == 154);
    pet_sleep_b_layout(false, &tcy, &bcy); assert(tcy == 136 && bcy == 172);
    puts("B content (printing vs not) + layout: PASS");

    // ---- 自动亮屏:变化 vs 第一次看到 ----
    const pet_state_t W[] = { PET_DONE, PET_FAIL, PET_PAUSE, PET_EMPTY };
    for (int i = 0; i < 4; i++) {
        assert(pet_wake_on_state(PET_PRINT, W[i]));            // 打印中 → 4 种:亮
        assert(!pet_wake_on_state(PET_STATE_COUNT, W[i]));     // 开机/重连后第一次看到:不亮
        assert(!pet_wake_on_state(W[i], W[i]));                // 没变:不亮
    }
    assert(pet_wake_on_state(PET_PAUSE, PET_EMPTY) && pet_wake_on_state(PET_EMPTY, PET_PAUSE));   // 暂停↔断料:每进入一次亮一次
    assert(!pet_wake_on_state(PET_IDLE, PET_PRINT) && !pet_wake_on_state(PET_DONE, PET_IDLE) && !pet_wake_on_state(PET_PAUSE, PET_PRINT));
    pet_state_t prev[PET_MAX_PRINTERS]; pet_wake_reset(prev);
    int pi;
    pet_state_t now1[PET_MAX_PRINTERS] = { PET_DONE, PET_PAUSE, PET_PRINT, PET_FAIL };
    assert(pet_wake_check(prev, now1, 4, &pi) == 0 && pi == -1);     // 第一轮:全是「第一次看到」
    pet_state_t now2[PET_MAX_PRINTERS] = { PET_DONE, PET_PAUSE, PET_PAUSE, PET_FAIL };
    assert(pet_wake_check(prev, now2, 4, &pi) == 1 && pi == 2);      // 第 3 台变成暂停:亮屏并切到它
    assert(pet_wake_check(prev, now2, 4, &pi) == 0 && pi == -1);     // 再来一轮没变化
    pet_state_t now3[PET_MAX_PRINTERS] = { PET_IDLE, PET_PAUSE, PET_EMPTY, PET_FAIL };
    assert(pet_wake_check(prev, now3, 4, &pi) == 1 && pi == -1);     // 断料:亮,但不是暂停 → 不额外切机(走现有自动跳转)
    pet_state_t now4[PET_MAX_PRINTERS] = { PET_PRINT, PET_PAUSE, PET_EMPTY, PET_FAIL };
    pet_wake_check(prev, now4, 4, &pi);
    pet_state_t now5[PET_MAX_PRINTERS] = { PET_DONE, PET_PAUSE, PET_EMPTY, PET_FAIL };
    assert(pet_wake_check(prev, now5, 4, &pi) == 1);                 // 完成
    pet_wake_reset(prev); assert(prev[0] == PET_STATE_COUNT && prev[3] == PET_STATE_COUNT);
    assert(pet_wake_check(prev, now5, 9, &pi) == 0);                 // n 越界按 PET_MAX_PRINTERS
    puts("auto-wake transitions (4 states, first-seen ignored, pause switches printer): PASS");

    // ---- 设置页 6 行布局 + 序号映射 + 文案 ----
    assert(SET_COUNT == 6);
    assert(SET_DEMO == 0 && SET_SLEEP == 1 && SET_FACE == 2 && SET_REPAIR == 3 && SET_BACK == 4 && SET_RESET == 5);
    const int Y[6] = { 54, 92, 130, 168, 206, 244 };
    for (int i = 0; i < SET_COUNT; i++) assert(pet_set_row_y(i) == Y[i]);
    assert(SET_ROW_H == 34 && SET_ROW_X == 20 && SET_ROW_W == 200 && SET_TXT_DX == 14 && SET_TXT_DY == 6);
    assert(pet_set_row_y(SET_RESET) + SET_ROW_H - 1 == 277 && 277 < SET_HINT_Y);        // 不压底部提示
    for (int i = 1; i < SET_COUNT; i++) assert(pet_set_row_y(i) - (pet_set_row_y(i - 1) + SET_ROW_H) == 4);   // 行间距 4
    assert(pet_set_row_y(0) + SET_TXT_DY == 60 && pet_set_row_y(SET_FACE) + SET_TXT_DY == 136);              // 文字绝对 y(UI 表)
    // 上下键走一遍:0→…→5 到头停住,不回绕
    int sel = 0; const int order[] = { SET_SLEEP, SET_FACE, SET_REPAIR, SET_BACK, SET_RESET, SET_RESET };
    for (int i = 0; i < 6; i++) { sel = pet_set_step(sel, 1, SET_COUNT); assert(sel == order[i]); }
    assert(!strcmp(pet_set_sleep_label(0), "休眠：关") && !strcmp(pet_set_sleep_label(2), "休眠：2 分钟") && !strcmp(pet_set_sleep_label(5), "休眠：5 分钟"));
    assert(!strcmp(pet_set_sleep_label(3), "休眠：2 分钟"));
    assert(!strcmp(pet_set_face_label(0), "熄屏画面：全黑") && !strcmp(pet_set_face_label(1), "熄屏画面：时钟") && !strcmp(pet_set_face_label(2), "熄屏画面：线线"));
    assert(!strcmp(pet_set_face_label(9), "熄屏画面：全黑"));
    puts("settings 6 rows layout + index mapping + labels: PASS");

    puts("test_sleep_v48 OK");
    return 0;
}
