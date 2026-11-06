// main/sleep_logic.c — 见 sleep_logic.h
#include "sleep_logic.h"
#include <stdio.h>
#include <string.h>

bool pet_sleep_allowed(ui_screen_t sc, bool portal_on, bool demo, uint8_t sleep_min) {
    if (sleep_min_sanitize(sleep_min) == SLEEP_MIN_OFF) return false;
    if (portal_on || demo) return false;
    switch (sc) {
    case UI_SETUP_QR: case UI_BOUND: case UI_REAUTH: case UI_RESET: return false;
    default: return true;   // UI_PET(首页/详情/耗材/设置)、UI_EXPIRY(到期提醒)
    }
}

void pet_sleep_init(pet_sleep_t *s, uint32_t now_ms) { s->asleep = false; s->idle_since_ms = now_ms; }
void pet_sleep_activity(pet_sleep_t *s, uint32_t now_ms) { s->idle_since_ms = now_ms; }
void pet_sleep_set_awake(pet_sleep_t *s, uint32_t now_ms) { s->asleep = false; s->idle_since_ms = now_ms; }

pet_sleep_ev_t pet_sleep_tick(pet_sleep_t *s, uint32_t now_ms, bool allowed, uint8_t sleep_min) {
    if (s->asleep) return PET_SLEEP_EV_NONE;
    uint8_t m = sleep_min_sanitize(sleep_min);
    if (!allowed || m == SLEEP_MIN_OFF) { s->idle_since_ms = now_ms; return PET_SLEEP_EV_NONE; }   // 暂停计时
    if ((uint32_t)(now_ms - s->idle_since_ms) >= (uint32_t)m * 60000u) { s->asleep = true; return PET_SLEEP_EV_ENTER; }
    return PET_SLEEP_EV_NONE;
}

pet_view_t pet_sleep_view_on_enter(pet_view_t v) { v.settings = false; return v; }

pet_sleep_actions_t pet_sleep_actions(uint8_t face) {
    pet_sleep_actions_t a = { 0 };
    a.stop_pet_anim = true;
    if (sleep_face_sanitize(face) == SLEEP_FACE_BLACK) {
        a.backlight_pct = 0; a.panel_sleep = true; a.pause_render = true;
    } else {
        a.backlight_pct = SLEEP_BL_DIM_PCT; a.sleep_screen = true; a.hide_status_bar = true;
    }
    return a;
}

bool pet_sleep_refresh_due(int last_key, int now_key, uint32_t last_ms, uint32_t now_ms) {
    if (now_key >= 0) return now_key != last_key;
    return (uint32_t)(now_ms - last_ms) >= SLEEP_REFRESH_MS || last_key >= 0;   // 刚失去对时(极少):也刷一次
}

bool pet_sleep_shift_due(int minute_key, int refresh_n) {
    if (minute_key >= 0) return minute_key % SLEEP_SHIFT_EVERY == 0;
    return refresh_n > 0 && refresh_n % SLEEP_SHIFT_EVERY == 0;
}

void pet_sleep_shift_xy(int pos, int *dx, int *dy) {
    static const int8_t T[4][2] = { { -SLEEP_SHIFT_PX, -SLEEP_SHIFT_PX }, { SLEEP_SHIFT_PX, -SLEEP_SHIFT_PX },
                                    { SLEEP_SHIFT_PX, SLEEP_SHIFT_PX }, { -SLEEP_SHIFT_PX, SLEEP_SHIFT_PX } };
    if (pos < 0) { *dx = 0; *dy = 0; return; }
    *dx = T[pos % 4][0]; *dy = T[pos % 4][1];
}

int pet_sleep_pick_printer(const pet_state_t *states, int n, int cur) {
    if (n > PET_MAX_PRINTERS) n = PET_MAX_PRINTERS;
    if (cur >= 0 && cur < n && states[cur] == PET_PRINT) return cur;
    for (int i = 0; i < n; i++) if (states[i] == PET_PRINT) return i;
    return -1;
}

void pet_sleep_b_layout(bool printing, int *time_cy, int *batt_cy) {
    *time_cy = printing ? 118 : 136;
    *batt_cy = printing ? 154 : 172;
}

bool pet_wake_on_state(pet_state_t prev, pet_state_t now) {
    if (prev >= PET_STATE_COUNT || prev == now) return false;
    return now == PET_DONE || now == PET_FAIL || now == PET_PAUSE || now == PET_EMPTY;
}

int pet_wake_check(pet_state_t *prev, const pet_state_t *now, int n, int *pause_idx) {
    int hits = 0;
    if (pause_idx) *pause_idx = -1;
    if (n > PET_MAX_PRINTERS) n = PET_MAX_PRINTERS;
    for (int i = 0; i < n; i++) {
        if (pet_wake_on_state(prev[i], now[i])) { hits++; if (now[i] == PET_PAUSE && pause_idx) *pause_idx = i; }
        prev[i] = now[i];
    }
    return hits;
}

void pet_wake_reset(pet_state_t *prev) { for (int i = 0; i < PET_MAX_PRINTERS; i++) prev[i] = PET_STATE_COUNT; }

const char *pet_set_sleep_label(uint8_t sleep_min) {
    switch (sleep_min_sanitize(sleep_min)) { case 0: return "休眠：关"; case 5: return "休眠：5 分钟"; default: return "休眠：2 分钟"; }
}
const char *pet_set_face_label(uint8_t sleep_face) {
    switch (sleep_face_sanitize(sleep_face)) { case SLEEP_FACE_CLOCK: return "熄屏画面：时钟"; case SLEEP_FACE_PET: return "熄屏画面：线线"; default: return "熄屏画面：全黑"; }
}

void pet_sleep_fmt_time(bool synced, int hour, int min, char *out, size_t len) {
    if (!out || !len) return;
    if (!synced || hour < 0 || hour > 23 || min < 0 || min > 59) snprintf(out, len, "--:--");
    else snprintf(out, len, "%02d:%02d", hour, min);
}

void pet_sleep_fmt_batt(int soc, char *out, size_t len) {
    if (!out || !len) return;
    if (soc < 0) snprintf(out, len, "--%%");
    else snprintf(out, len, "%d%%", soc > 100 ? 100 : soc);
}

void pet_sleep_fmt_pct(int pct, char *out, size_t len) {
    if (!out || !len) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    snprintf(out, len, "%d%%", pct);
}

void pet_sleep_compose_view(uint8_t face, const char *time, int soc, bool printing,
                            const char *name, const char *product, int idx, int pct,
                            bool name_ok, pet_sleep_view_t *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (time && time[0]) snprintf(out->time, sizeof out->time, "%s", time);
    else snprintf(out->time, sizeof out->time, "--:--");
    pet_sleep_fmt_batt(soc, out->batt, sizeof out->batt);
    face = sleep_face_sanitize(face);
    if (face == SLEEP_FACE_BLACK) { out->pause_render = true; return; }
    out->uses_sleep_screen = true;
    out->show_time = true;
    out->show_batt = true;
    if (face == SLEEP_FACE_PET) {
        out->show_pet = true;
        out->time_cy = 222;
        out->batt_cy = 252;
        return;
    }
    out->show_print_row = printing;
    pet_sleep_b_layout(printing, &out->time_cy, &out->batt_cy);
    if (printing) {
        pet_fmt_display_name(out->name, sizeof out->name, name, product, idx, name_ok);
        pet_sleep_fmt_pct(pct, out->pct, sizeof out->pct);
    }
}

bool pet_sleep_content_changed(bool was_printing, int was_idx, int was_pct,
                               const char *was_name, const char *was_product,
                               bool printing, int idx, int pct,
                               const char *name, const char *product) {
    if (was_printing != printing || was_idx != idx || was_pct != pct) return true;
    if (!was_name) was_name = "";
    if (!name) name = "";
    if (!was_product) was_product = "";
    if (!product) product = "";
    return strcmp(was_name, name) != 0 || strcmp(was_product, product) != 0;
}

