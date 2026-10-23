// Host test for main/pet_logic.c
#include "pet_logic.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static printer_status_t st(const char *g, uint32_t err) {
    printer_status_t s; printer_status_reset(&s); s.valid = true;
    snprintf(s.gcode_state, sizeof s.gcode_state, "%s", g); s.print_error = err; return s;
}
int main(void) {
    assert(pet_is_runout_print_error(0x07008011));
    assert(pet_is_runout_print_error(0x07FF8011));
    assert(pet_is_runout_print_error(0x12FF8011));
    assert(pet_is_runout_print_error(0x18008011));
    assert(!pet_is_runout_print_error(0x07008004));
    assert(!pet_is_runout_print_error(0));
    assert(pet_is_runout_hms(0x07007000, 0x00020001));
    assert(!pet_is_runout_hms(0x03007000, 0x00020001));
    printer_status_t s;
    s = st("RUNNING", 0); assert(pet_state_from_status(&s) == PET_PRINT);
    s = st("PAUSE", 0); assert(pet_state_from_status(&s) == PET_PAUSE);
    s = st("PAUSE", 0x07018011); assert(pet_state_from_status(&s) == PET_EMPTY);
    s = st("FAILED", 0); assert(pet_state_from_status(&s) == PET_FAIL);
    s = st("IDLE", 0); assert(pet_state_from_status(&s) == PET_IDLE);
    printer_status_reset(&s); assert(pet_state_from_status(&s) == PET_IDLE);
    pet_machine_t m; pet_machine_init(&m);
    s = st("FINISH", 0); assert(pet_machine_update(&m, &s) == PET_IDLE); // stale finish at boot
    s = st("RUNNING", 0); assert(pet_machine_update(&m, &s) == PET_PRINT);
    s = st("FINISH", 0); assert(pet_machine_update(&m, &s) == PET_DONE);
    assert(pet_machine_update(&m, &s) == PET_DONE);
    assert(pet_machine_ack(&m));
    assert(pet_machine_update(&m, &s) == PET_IDLE);
    assert(!pet_machine_ack(&m));
    for (int i = 0; i < 6; i++) {
        pet_machine_t mm; pet_machine_init(&mm); mm.seen_active = true;
        pet_fake_status(i, 0, &s); assert(pet_machine_update(&mm, &s) == (pet_state_t)i);
    }
    char b[32]; pet_format_remaining(72, b, sizeof b); assert(strcmp(b, "还剩 1小时12分") == 0);
    pet_format_remaining(8, b, sizeof b); assert(strcmp(b, "还剩 8分钟") == 0);
    assert(pet_parse_tray_color("FF0000FF") == 0xFF0000);
    assert(pet_parse_tray_color("zz") == 0);
    assert(pet_parse_tray_color("000000FF") == 0x010101);
    assert(pet_model_top_y(0, 262, 216) == 262);
    assert(pet_model_top_y(100, 262, 216) == 216);
    assert(pet_model_top_y(50, 262, 216) == 239);
    assert(pet_model_top_y(150, 262, 216) == 216);
    assert(216 - 164 == 52);  // print sprite nozzle anchor -> head top at y=52
    assert(!pet_token_needs_refresh(0, 1800000000, 86400));
    assert(!pet_token_needs_refresh(1800000000, 1000, 86400));
    assert(pet_token_needs_refresh(1800000000, 1800000000 - 100, 7 * 86400));
    assert(!pet_token_needs_refresh(1800000000, 1700000000, 7 * 86400));
    assert(pet_token_days_left(0, 1800000000) == -1);
    assert(pet_token_days_left(1800000000, 1800000001) == 0);
    assert(pet_token_days_left(1800000000, 1800000000 - 86400 * 3) == 3);
    assert(pet_token_days_left(1800000000, 1800000000 - 86400 * 2 - 5) == 3);
    assert(pet_expiry_should_remind(3, 20000, 19999));
    assert(!pet_expiry_should_remind(3, 20000, 20000));
    assert(!pet_expiry_should_remind(4, 20000, 0));
    assert(!pet_expiry_should_remind(0, 20000, 0));
    assert(pet_token_minutes_left(1800000600, 1800000000) == 10);
    assert(pet_token_minutes_left(1800000600, 1800000600 - 170) == 3);
    assert(pet_token_minutes_left(1800000600, 1800000601) == 0);
    // recolor: luminance clamp + fallback
    assert(pet_recolor_rgb(0) == PET_RECOLOR_FALLBACK_RGB);            // no AMS colour -> light grey
    assert(PET_RECOLOR_FALLBACK_RGB != 0xFFFFFFu);                     // must differ from white outline
    assert(pet_recolor_rgb(0xFF8C3C) == 0xFF8C3C);                     // bright orange untouched
    assert(pet_recolor_rgb(0x00AE42) == 0x00AE42);                     // Bambu green untouched
    assert(pet_recolor_rgb(0xFFFFFF) == 0xFFFFFF);
    // #C000C0 shade: *0.7; near-black brightens
    { uint32_t s = pet_recolor_shade_rgb(0xFF8C3C);
      assert(((s>>16)&255) == (0xFF*7)/10);
      assert(pet_luma(pet_recolor_shade_rgb(0)) > pet_luma(PET_RECOLOR_FALLBACK_RGB)*6/10); }
    { uint32_t face = pet_recolor_rgb(0x010101), side = pet_recolor_shade_rgb(0x010101);
      assert(pet_luma(side) > pet_luma(face)); }
    assert(pet_luma(0x000000) == 0 && pet_luma(0xFFFFFF) == 255);
    {   // every dark colour ends at >= 25% luma, never overshoots much, keeps hue ordering
        static const uint32_t dark[] = { 0x010101, 0x000000 + 1, 0x202020, 0x3F3F3F, 0x1A1A40, 0x400000, 0x000040, 0x0A2A0A, 0x303030 };
        for (unsigned i = 0; i < sizeof dark / sizeof dark[0]; i++) {
            uint32_t o = pet_recolor_rgb(dark[i]); int y = pet_luma(o);
            assert(y >= PET_RECOLOR_MIN_LUMA && y <= PET_RECOLOR_MIN_LUMA + 3);
            for (int sh = 0; sh <= 16; sh += 8) assert(((o >> sh) & 0xFF) >= ((dark[i] >> sh) & 0xFF));  // only brightens
        }
        uint32_t navy = pet_recolor_rgb(0x000040);
        assert((navy & 0xFF) > ((navy >> 16) & 0xFF));                 // dark blue stays bluish
    }
    for (uint32_t v = 1; v < 0x1000000; v += 0x010307) {               // sweep: clamp holds everywhere
        uint32_t o = pet_recolor_rgb(v);
        assert(pet_luma(o) >= PET_RECOLOR_MIN_LUMA);
        if (pet_luma(v) >= PET_RECOLOR_MIN_LUMA) assert(o == v);
    }
    {   // duration formatting
        char b[24];
        pet_format_duration(192, b, sizeof b); assert(!strcmp(b, "3h 12m"));
        pet_format_duration(5, b, sizeof b);   assert(!strcmp(b, "0h 05m"));
        pet_format_duration(0, b, sizeof b);   assert(!strcmp(b, "0h 00m"));
        pet_format_duration(60 * 25 + 1, b, sizeof b); assert(!strcmp(b, "25h 01m"));
        pet_format_duration(-1, b, sizeof b);  assert(!strcmp(b, "--"));
    }
    {   // job tracker: seen from the start -> elapsed known
        const int64_t T = 1760000000;
        pet_job_t j; pet_job_init(&j);
        printer_status_t p; printer_status_reset(&p); p.valid = true;
        snprintf(p.job_id, sizeof p.job_id, "690732217");
        snprintf(p.gcode_state, sizeof p.gcode_state, "PREPARE"); p.percent = 0;
        assert(pet_job_update(&j, &p, T));
        snprintf(p.gcode_state, sizeof p.gcode_state, "RUNNING"); p.percent = 40;
        assert(!pet_job_update(&j, &p, T + 3600));                 // same job, nothing to save
        assert(pet_job_elapsed_min(&j, &p) == -1);                 // not finished yet
        snprintf(p.gcode_state, sizeof p.gcode_state, "FINISH"); p.percent = 100;
        assert(pet_job_update(&j, &p, T + 192 * 60 + 20));
        assert(pet_job_elapsed_min(&j, &p) == 192);
        assert(!pet_job_update(&j, &p, T + 999999));               // end not overwritten by later pushes
        assert(pet_job_elapsed_min(&j, &p) == 192);
        // "reboot": record restored from NVS (struct copy) still matches the same job
        pet_job_t r = j; assert(pet_job_elapsed_min(&r, &p) == 192);
        // a different job on screen -> stored record does not apply
        printer_status_t q = p; snprintf(q.job_id, sizeof q.job_id, "111"); assert(pet_job_elapsed_min(&j, &q) == -1);
    }
    {   // unknown start: first seen mid-print (booted at 50%, no record) -> elapsed unknown even after FINISH
        const int64_t T = 1760000000;
        pet_job_t j; pet_job_init(&j);
        printer_status_t p; printer_status_reset(&p); p.valid = true;
        snprintf(p.job_id, sizeof p.job_id, "42");
        snprintf(p.gcode_state, sizeof p.gcode_state, "RUNNING"); p.percent = 50;
        assert(pet_job_update(&j, &p, T)); assert(j.start == 0);
        snprintf(p.gcode_state, sizeof p.gcode_state, "FINISH");
        assert(!pet_job_update(&j, &p, T + 600));
        assert(pet_job_elapsed_min(&j, &p) == -1);
        // clock not synced at start -> unknown too
        pet_job_init(&j); snprintf(p.gcode_state, sizeof p.gcode_state, "RUNNING"); p.percent = 0;
        pet_job_update(&j, &p, 1000); assert(j.start == 0);
        // no job id (partial pushes before first push_all) -> ignored
        pet_job_init(&j); printer_status_t e; printer_status_reset(&e); e.valid = true;
        snprintf(e.gcode_state, sizeof e.gcode_state, "RUNNING"); assert(!pet_job_update(&j, &e, T));
        assert(e.elapsed_min == -1);
    }
    {   // ---- v4.2 ----
        char t[24];
        pet_format_temp(39, 0, t, sizeof t); assert(!strcmp(t, "39°C"));
        pet_format_temp(36, 220, t, sizeof t); assert(!strcmp(t, "36→220°C"));
        pet_format_temp(219.4f, 220, t, sizeof t); assert(!strcmp(t, "219°C"));   // 到温后只显示当前
        pet_format_temp(60, 55, t, sizeof t); assert(!strcmp(t, "60°C"));          // 降温中
        assert(!strcmp(pet_color_name(0xFFFFFF), "白色")); assert(!strcmp(pet_color_name(0x010101), "黑色"));
        assert(!strcmp(pet_color_name(0xDE4343), "红色")); assert(!strcmp(pet_color_name(0x00AE42), "绿色"));
        assert(!strcmp(pet_color_name(0x3C78E6), "蓝色")); assert(!strcmp(pet_color_name(0xF5C542), "黄色"));
        assert(!strcmp(pet_color_name(0x808080), "灰色")); assert(!strcmp(pet_color_name(0), ""));
        pet_tray_label(128, 0, t, sizeof t); assert(!strcmp(t, "HT-A"));
        pet_tray_label(0, 1, t, sizeof t); assert(!strcmp(t, "A2"));
        pet_tray_label(254, 0, t, sizeof t); assert(!strcmp(t, "外挂"));
        printer_status_t q = st("RUNNING", 0);
        assert(!strcmp(pet_status_word(&q, PET_PRINT), "打印中"));
        assert(!strcmp(pet_status_word(&q, PET_DONE), "成功"));
        q = st("FINISH", 0); assert(!strcmp(pet_status_word(&q, PET_IDLE), "成功"));
        q = st("PREPARE", 0); assert(!strcmp(pet_status_word(&q, PET_PRINT), "准备中"));
        // 自动跳转
        pet_switch_t w; pet_switch_init(&w, 3);
        pet_state_t S[4] = { PET_IDLE, PET_EMPTY, PET_IDLE, PET_IDLE };
        assert(pet_switch_tick(&w, S, 0) == -1);              // 首次观察(开机时已断料)不跳
        S[1] = PET_PRINT; assert(pet_switch_tick(&w, S, 100) == -1);
        S[1] = PET_DONE;  assert(pet_switch_tick(&w, S, 1000) == -1);    // 事件 -> 停 3 秒
        assert(pet_switch_tick(&w, S, 3999) == -1);
        S[2] = PET_FAIL;  assert(pet_switch_tick(&w, S, 3500) == -1);    // 同时来了更严重的:顶替,不延长
        assert(pet_switch_tick(&w, S, 4000) == 2);
        pet_switch_set_cur(&w, 2, 4000, false);
        assert(pet_switch_tick(&w, S, 4100) == -1);           // 1 号的完成还排着,但至少再等 3 秒
        assert(pet_switch_tick(&w, S, 6999) == -1);
        assert(pet_switch_tick(&w, S, 7100) == 1);
        pet_switch_set_cur(&w, 1, 7100, false);
        assert(pet_switch_tick(&w, S, 20000) == -1);          // 没有新事件就不再跳(不来回抖)
        S[0] = PET_PRINT; pet_switch_tick(&w, S, 20100); S[0] = PET_FAIL; pet_switch_tick(&w, S, 20200);
        S[0] = PET_PRINT; assert(pet_switch_tick(&w, S, 30000) == -1);    // 事件在等待期间消失:取消
        S[2] = PET_PRINT; pet_switch_tick(&w, S, 30100); S[2] = PET_EMPTY; pet_switch_tick(&w, S, 30200);
        pet_switch_set_cur(&w, 0, 31000, true);               // 用户手动切走:重新计时
        assert(pet_switch_tick(&w, S, 33300) == -1);
        assert(pet_switch_tick(&w, S, 34100) == 2);
        // 演示第二台
        printer_status_t p2; pet_fake_status_p2(0, &p2); assert(p2.n_nozzles == 1 && !strcmp(p2.fil_slot, "A2"));
        pet_fake_status(0, 0, &p2); assert(p2.n_nozzles == 2 && p2.fil_humidity == 22 && !strcmp(p2.fil_slot, "HT-A"));
    }
    puts("test_pet_logic: OK");
    return 0;
}
