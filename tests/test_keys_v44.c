// Host test for the v4.4 key state machine / page cycle / bottom hints (main/pet_logic.c)
#include "pet_logic.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static pet_okkey_t K;
static pet_ok_ev_t click(uint32_t t_down, uint32_t t_up, bool dbl) {
    pet_ok_ev_t a = pet_okkey_press(&K, t_down); assert(a == PET_OK_NONE);
    return pet_okkey_release(&K, t_up, dbl);
}
int main(void) {
    assert(PET_DBL_MS == 250);
    // --- double-click enabled (>= 2 printers, not demo) ---
    assert(pet_dbl_enabled(false, 2) && pet_dbl_enabled(false, 4));
    pet_okkey_init(&K);
    assert(click(0, 80, true) == PET_OK_NONE);              // single: held back for the window
    assert(pet_okkey_tick(&K, 80 + 250) == PET_OK_NONE);    // exactly 250 ms: still waiting
    assert(pet_okkey_tick(&K, 80 + 251) == PET_OK_SINGLE);
    assert(pet_okkey_tick(&K, 1000) == PET_OK_NONE);        // fired once only
    // double: second press 240 ms after first release (slow double, would have failed at 180 ms)
    assert(click(2000, 2080, true) == PET_OK_NONE);
    assert(pet_okkey_tick(&K, 2300) == PET_OK_NONE);
    assert(pet_okkey_press(&K, 2320) == PET_OK_NONE);
    assert(pet_okkey_tick(&K, 2400) == PET_OK_NONE);        // second press held past the window: no stray single
    assert(pet_okkey_release(&K, 2420, true) == PET_OK_DOUBLE);
    assert(pet_okkey_tick(&K, 3000) == PET_OK_NONE);
    // two presses 260 ms apart = two singles
    assert(click(4000, 4050, true) == PET_OK_NONE);
    assert(pet_okkey_press(&K, 4050 + 260) == PET_OK_SINGLE);   // late tick: first single flushed on next press
    assert(pet_okkey_release(&K, 4400, true) == PET_OK_NONE);
    assert(pet_okkey_tick(&K, 4651) == PET_OK_SINGLE);
    // long press: no single/double on release; cancels a pending single
    assert(pet_okkey_press(&K, 5000) == PET_OK_NONE); pet_okkey_long(&K);
    assert(pet_okkey_release(&K, 6000, true) == PET_OK_NONE);
    assert(pet_okkey_tick(&K, 7000) == PET_OK_NONE);
    assert(click(8000, 8050, true) == PET_OK_NONE);
    assert(pet_okkey_press(&K, 8100) == PET_OK_NONE); pet_okkey_long(&K);   // click then press-and-hold = settings only
    assert(pet_okkey_release(&K, 9500, true) == PET_OK_NONE);
    assert(pet_okkey_tick(&K, 9900) == PET_OK_NONE);
    assert(pet_okkey_release(&K, 9950, true) == PET_OK_NONE);           // stray release ignored
    // --- double-click disabled: demo mode, or only one printer ---
    assert(!pet_dbl_enabled(true, 2) && !pet_dbl_enabled(true, 1) && !pet_dbl_enabled(false, 1) && !pet_dbl_enabled(false, 0));
    pet_okkey_init(&K);
    assert(click(0, 60, false) == PET_OK_SINGLE);           // immediate, no 250 ms wait
    assert(click(100, 150, false) == PET_OK_SINGLE);        // fast second press = second single (demo: next state twice)
    assert(pet_okkey_tick(&K, 1000) == PET_OK_NONE);
    assert(pet_okkey_press(&K, 2000) == PET_OK_NONE); pet_okkey_long(&K);
    assert(pet_okkey_release(&K, 3000, false) == PET_OK_NONE);   // long still = settings
    // --- pages: up/down cycle home -> detail -> filament ---
    assert(pet_page_step(0, 1) == 1 && pet_page_step(1, 1) == 2 && pet_page_step(2, 1) == 0);
    assert(pet_page_step(0, -1) == 2 && pet_page_step(2, -1) == 1 && pet_page_step(1, -1) == 0);
    // --- filament scroll-then-switch: 4 AMS + 2 HT + ext overflows; walk down to the end, then the page must change ---
    pet_ams_t a; pet_ams_reset(&a); a.valid = true;
    int ids[] = { 0, 1, 2, 3, 128, 129 };
    for (int u = 0; u < 6; u++) { a.u[u].id = ids[u]; a.u[u].nslot = u < 4 ? 4 : 1; a.u[u].first = u < 4 ? u * 4 : 16 + (u - 4); }
    a.n_units = 6; a.n_ext = 2;
    int first = 0, steps = 0, f;
    while ((f = pet_ams_scroll(&a, first, 1, PET_AMS_VIEW_H)) >= 0) { first = f; steps++; }
    assert(steps == 3);                                    // 3 scrolls down, 4th press leaves the page
    assert(pet_ams_scroll(&a, first, 1, PET_AMS_VIEW_H) < 0);
    steps = 0; while ((f = pet_ams_scroll(&a, first, -1, PET_AMS_VIEW_H)) >= 0) { first = f; steps++; }
    assert(steps == 3 && first == 0);
    pet_ams_t k9; pet_fake_ams(0, &k9);                    // demo printer's 9 slots fit one screen: up/down switch page at once
    assert(pet_ams_scroll(&k9, 0, 1, PET_AMS_VIEW_H) < 0 && pet_ams_scroll(&k9, 0, -1, PET_AMS_VIEW_H) < 0);
    // --- hints ---
    char b[96];
    assert(!strcmp(pet_hint_text(0, false, 2, 0, b, sizeof b), "上下键切页 · 双击中键换机器"));
    assert(!strcmp(pet_hint_text(1, false, 4, 0, b, sizeof b), "上下键切页 · 双击中键换机器"));
    assert(!strcmp(pet_hint_text(2, false, 3, 0, b, sizeof b), "上下键翻页/切页 · 双击中键换机器"));
    assert(!strcmp(pet_hint_text(0, false, 1, 0, b, sizeof b), "上下键切页"));      // one printer: drop the double-click part
    assert(!strcmp(pet_hint_text(1, false, 1, 0, b, sizeof b), "上下键切页"));
    assert(!strcmp(pet_hint_text(2, false, 1, 0, b, sizeof b), "上下键翻页/切页"));
    for (int p = 0; p < 3; p++) assert(!strcmp(pet_hint_text(p, true, 2, 3, b, sizeof b), "中键切状态 4/6 · 上下键切页"));
    assert(!strcmp(pet_hint_text(2, true, 1, 5, b, sizeof b), "中键切状态 6/6 · 上下键切页"));
    // --- PM v4.4 final: hints only on detail/filament (always on), demo badge only on home ---
    assert(!pet_hint_on_page(0) && pet_hint_on_page(1) && pet_hint_on_page(2));
    assert(pet_demo_badge_on_page(0) && !pet_demo_badge_on_page(1) && !pet_demo_badge_on_page(2));
    // --- dots: only with >= 2 printers (demo is 1 printer since v4.4) ---
    assert(!pet_dots_visible(0) && !pet_dots_visible(1));
    assert(pet_dots_visible(2) && pet_dots_visible(4));
    puts("test_keys_v44: OK");
    return 0;
}
