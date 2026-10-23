// main/pet_logic.c
#include "pet_logic.h"
#include <stdio.h>
#include <string.h>

void printer_status_reset(printer_status_t *s) {
    memset(s, 0, sizeof(*s));
    s->percent = s->remaining_min = s->layer = s->total_layer = -1;
    s->elapsed_min = -1;
    s->fil_humidity = -1;
}

void pet_job_init(pet_job_t *j) { memset(j, 0, sizeof *j); }

static bool job_active(const char *g) {
    return !strcmp(g, "RUNNING") || !strcmp(g, "PREPARE") || !strcmp(g, "PAUSE") || !strcmp(g, "SLICING");
}

bool pet_job_update(pet_job_t *j, const printer_status_t *s, int64_t now) {
    if (!s->valid || !s->job_id[0]) return false;
    bool tv = now >= PET_TIME_VALID;
    if (job_active(s->gcode_state)) {
        if (strcmp(j->job, s->job_id) != 0) {          // 新的一次打印
            snprintf(j->job, sizeof j->job, "%s", s->job_id);
            bool at_start = !strcmp(s->gcode_state, "PREPARE") || (s->percent >= 0 && s->percent <= 1);
            j->start = (tv && at_start) ? now : 0;
            j->end = 0;
            return true;
        }
        if (j->end) { j->end = 0; return true; }        // 同一 job 失败后重试/恢复
        return false;
    }
    if ((!strcmp(s->gcode_state, "FINISH") || !strcmp(s->gcode_state, "FAILED")) &&
        !strcmp(j->job, s->job_id) && j->end == 0 && j->start > 0 && tv) {
        j->end = now;
        return true;
    }
    return false;
}

int pet_job_elapsed_min(const pet_job_t *j, const printer_status_t *s) {
    if (!s->job_id[0] || strcmp(j->job, s->job_id) || j->start <= 0 || j->end <= j->start) return -1;
    return (int)((j->end - j->start + 30) / 60);
}

void pet_format_duration(int min, char *buf, size_t len) {
    if (min < 0) { snprintf(buf, len, "--"); return; }
    snprintf(buf, len, "%dh %02dm", min / 60, min % 60);
}

// 拓竹 Wiki HMS:xxyy-8011 = "filament ran out"。0700..0703/07FF = AMS,
// 1200..1203/12FF = AMS lite,1800../18FF = AMS-HT;外挂料盘为 0300-8001 族之外,此处不猜。
static bool runout_module(uint32_t hi) {
    uint32_t mod = hi >> 8;
    return mod == 0x07 || mod == 0x12 || mod == 0x18;
}

bool pet_is_runout_print_error(uint32_t code) {
    if (code == 0) return false;
    return (code & 0xFFFF) == 0x8011 && runout_module(code >> 16);
}

bool pet_is_runout_hms(uint32_t attr, uint32_t code) {
    // HMS 全码 MMSS_7000_0002_0001 = "AMS X slot Y filament has run out"(MM=07/12/18)。
    if (!runout_module(attr >> 16)) return false;
    if ((attr & 0xF000) == 0x7000 && code == 0x00020001) return true;
    // 部分固件把 print_error 形式的 8011 也放进 hms
    return (attr & 0xFFFF) == 0x8011 || (code & 0xFFFF) == 0x8011;
}

pet_state_t pet_state_from_status(const printer_status_t *s) {
    if (!s || !s->valid) return PET_IDLE;
    const char *g = s->gcode_state;
    bool runout = s->hms_runout || pet_is_runout_print_error(s->print_error);
    if (strcmp(g, "FAILED") == 0) return PET_FAIL;
    if (strcmp(g, "PAUSE") == 0) return runout ? PET_EMPTY : PET_PAUSE;
    if (strcmp(g, "RUNNING") == 0 || strcmp(g, "PREPARE") == 0 || strcmp(g, "SLICING") == 0)
        return runout ? PET_EMPTY : PET_PRINT;
    if (strcmp(g, "FINISH") == 0) return PET_DONE;
    return PET_IDLE;
}

void pet_machine_init(pet_machine_t *m) {
    m->shown = PET_IDLE; m->done_acked = false; m->seen_active = false;
}

pet_state_t pet_machine_update(pet_machine_t *m, const printer_status_t *s) {
    pet_state_t raw = pet_state_from_status(s);
    if (raw == PET_PRINT || raw == PET_PAUSE || raw == PET_EMPTY) {
        m->seen_active = true; m->done_acked = false;
    }
    if (raw == PET_DONE) {
        if (m->done_acked || !m->seen_active) raw = PET_IDLE;
    } else if (raw == PET_IDLE || raw == PET_FAIL) {
        m->done_acked = false;
        if (raw == PET_FAIL) m->seen_active = true;
    }
    m->shown = raw;
    return raw;
}

bool pet_machine_ack(pet_machine_t *m) {
    if (m->shown != PET_DONE) return false;
    m->done_acked = true; m->seen_active = false; m->shown = PET_IDLE;
    return true;
}

void pet_fake_status(int step, uint32_t tick, printer_status_t *o) {
    printer_status_reset(o);
    o->valid = true;
    static const char *G[] = { "IDLE", "RUNNING", "FINISH", "PAUSE", "PAUSE", "FAILED" };
    step = ((step % 6) + 6) % 6;
    snprintf(o->gcode_state, sizeof(o->gcode_state), "%s", G[step]);
    snprintf(o->subtask, sizeof(o->subtask), "benchy_h2c");
    o->nozzle = step == 0 ? 28 : 220; o->nozzle_target = step == 0 ? 0 : 220;
    o->bed = step == 0 ? 26 : 55; o->bed_target = step == 0 ? 0 : 55;
    o->total_layer = 240;
    int pct = step == 1 ? (int)(30 + (tick / 2) % 70) : step == 2 ? 100 : 68;
    if (step == 0) pct = 0;
    o->percent = pct;
    o->layer = o->total_layer * pct / 100;
    o->remaining_min = step == 2 ? 0 : (100 - pct) * 72 / 32;
    static const uint32_t COL[] = { 0xE64646, 0xE64646, 0x3C78E6, 0xF5C542, 0xFAFAFA, 0x00AE42 };
    o->fil_rgb = COL[step];
    if (step == 2) o->elapsed_min = 192;          // 演示:用时 3h 12m
    if (step == 4) o->print_error = 0x07018011;   // AMS A 第 2 格缺料
    if (step == 5) o->print_error = 0x03008003;   // 演示用喷嘴类错误
    // v4.2 详情页演示数据(H2C 双喷嘴,右喷嘴在用)
    o->n_nozzles = 2; o->active_nozzle = 1;
    o->noz[0] = step == 0 ? 36 : 38; o->noz_t[0] = 0;
    o->noz[1] = o->nozzle; o->noz_t[1] = o->nozzle_target;
    o->has_chamber = true; o->chamber_has_target = true; o->chamber = step == 0 ? 33 : 41;
    snprintf(o->fil_type, sizeof o->fil_type, "PLA");
    snprintf(o->fil_slot, sizeof o->fil_slot, "HT-A");
    o->fil_humidity = 22;
}

void pet_fake_status_p2(uint32_t tick, printer_status_t *o) {
    printer_status_reset(o);
    o->valid = true;
    snprintf(o->gcode_state, sizeof o->gcode_state, "RUNNING");
    snprintf(o->subtask, sizeof o->subtask, "(示例) P1S 打印任务");
    o->percent = (int)(20 + (tick / 4) % 70); o->total_layer = 120; o->layer = o->total_layer * o->percent / 100;
    o->remaining_min = (100 - o->percent) * 2;
    o->n_nozzles = 1; o->active_nozzle = 0;
    o->nozzle = o->noz[0] = 220; o->nozzle_target = o->noz_t[0] = 220;
    o->bed = 55; o->bed_target = 55;
    o->fil_rgb = 0x3C78E6;
    snprintf(o->fil_type, sizeof o->fil_type, "PETG");
    snprintf(o->fil_slot, sizeof o->fil_slot, "A2");
    o->fil_humidity = -1;
}

void pet_format_remaining(int min, char *buf, size_t len) {
    if (min < 0) snprintf(buf, len, "--");
    else if (min >= 60) snprintf(buf, len, "还剩 %d小时%d分", min / 60, min % 60);
    else snprintf(buf, len, "还剩 %d分钟", min);
}

const char *pet_error_text(uint32_t code) {
    if (code == 0) return "打印失败";
    if (pet_is_runout_print_error(code)) return "缺料";
    uint32_t mod = code >> 24;
    if (mod == 0x03) return "打印头卡住";       // 主控/运动 & 工具头类
    if (mod == 0x05) return "第一层检测异常";
    if (mod == 0x07 || mod == 0x12 || mod == 0x18) return "耗材缠绕";
    return "打印失败";
}

uint32_t pet_parse_tray_color(const char *s) {
    if (!s || strlen(s) < 6) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 6; i++) {
        char c = s[i]; int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;
        v = (v << 4) | (uint32_t)d;
    }
    return v ? v : 0x010101;  // 纯黑料仍算"已知"
}

bool pet_token_needs_refresh(int64_t exp, int64_t now, int64_t margin) {
    if (exp <= 0 || now < 1700000000) return false;  // 过期时间未知或还没对时
    return now >= exp - margin;
}

int pet_model_top_y(int percent, int bed_y, int top_y) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    return bed_y - (bed_y - top_y) * percent / 100;
}

int pet_token_days_left(int64_t exp, int64_t now) {
    if (exp <= 0 || now < 1700000000) return -1;
    if (now >= exp) return 0;
    return (int)((exp - now + 86399) / 86400);
}

bool pet_expiry_should_remind(int days_left, int32_t today, int32_t last_reminded_day) {
    return days_left >= 1 && days_left <= 3 && today != last_reminded_day;
}

int pet_token_minutes_left(int64_t exp, int64_t now) {
    if (exp <= 0 || now < 1700000000) return -1;
    if (now >= exp) return 0;
    return (int)((exp - now + 59) / 60);
}

int pet_luma(uint32_t rgb) {
    int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return (54 * r + 183 * g + 19 * b + 128) >> 8;   // 0.2126/0.7152/0.0722 in /256
}

uint32_t pet_recolor_rgb(uint32_t fil_rgb) {
    if (fil_rgb == 0) return PET_RECOLOR_FALLBACK_RGB;
    int y = pet_luma(fil_rgb);
    if (y >= PET_RECOLOR_MIN_LUMA) return fil_rgb & 0xFFFFFF;
    // c' = c + (255-c)*t, t = (64-y)/(255-y): luma is linear in t, so this lands on 64 (+rounding).
    int num = PET_RECOLOR_MIN_LUMA - y, den = 255 - y;
    uint32_t out = 0;
    for (int sh = 16; sh >= 0; sh -= 8) {
        int c = (fil_rgb >> sh) & 0xFF;
        c += ((255 - c) * num + den - 1) / den;        // round up so we never fall short of 25%
        out |= (uint32_t)(c > 255 ? 255 : c) << sh;
    }
    return out;
}

uint32_t pet_recolor_shade_rgb(uint32_t fil_rgb) {
    // 先走主换色(兜底/亮度下限),再对 #C000C0 阴影键做像素级明暗。
    uint32_t base = pet_recolor_rgb(fil_rgb);
    bool near_black = (fil_rgb == 0) || (pet_luma(fil_rgb) < PET_RECOLOR_MIN_LUMA);
    uint32_t out = 0;
    for (int sh = 16; sh >= 0; sh -= 8) {
        int c = (base >> sh) & 0xFF;
        if (near_black) c = (c * 10 + 6) / 7;          // 提亮 ≈ /0.7
        else c = (c * 7) / 10;                         // 压暗 *0.7
        if (c > 255) c = 255;
        out |= (uint32_t)c << sh;
    }
    return out;
}

// ======================= v4.2 =======================
void pet_format_temp(float cur, float target, char *buf, size_t len) {
    int c = (int)(cur + (cur >= 0 ? 0.5f : -0.5f)), t = (int)(target + 0.5f);
    if (t > 0 && c < t - 2) snprintf(buf, len, "%d→%d°C", c, t);
    else snprintf(buf, len, "%d°C", c);
}

const char *pet_status_word(const printer_status_t *s, pet_state_t st) {
    switch (st) {
    case PET_DONE: return "成功";
    case PET_PAUSE: return "暂停";
    case PET_EMPTY: return "断料";
    case PET_FAIL: return "失败";
    case PET_PRINT: return (s && !strcmp(s->gcode_state, "PREPARE")) ? "准备中" : "打印中";
    default:
        if (s && !strcmp(s->gcode_state, "FINISH")) return "成功";   // 已确认过的完成:详情页仍写成功
        return "空闲";
    }
}

const char *pet_color_name(uint32_t rgb) {
    if (rgb == 0) return "";
    int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int d = mx - mn;
    if (d < 28 || (mx > 0 && d * 100 / mx < 14)) {           // 低饱和:黑白灰
        if (mx >= 200) return "白色";
        if (mx <= 50) return "黑色";
        return "灰色";
    }
    int h;                                                   // 0..359
    if (mx == r) h = (60 * (g - b) / d + 360) % 360;
    else if (mx == g) h = 60 * (b - r) / d + 120;
    else h = 60 * (r - g) / d + 240;
    if (mx < 140 && h >= 10 && h < 45) return "棕色";
    if (h < 12 || h >= 340) return (mn > 150) ? "粉色" : "红色";
    if (h < 40) return "橙色";
    if (h < 70) return "黄色";
    if (h < 160) return "绿色";
    if (h < 200) return "青色";
    if (h < 255) return "蓝色";
    if (h < 300) return "紫色";
    return "粉色";
}

void pet_tray_label(int ams_id, int slot, char *buf, size_t len) {
    if (ams_id == 254 || ams_id == 255) snprintf(buf, len, "外挂");
    else if (ams_id >= 128 && ams_id < 136) snprintf(buf, len, "HT-%c", 'A' + (ams_id - 128));
    else if (ams_id >= 0 && ams_id < 8 && slot >= 0 && slot < 4) snprintf(buf, len, "%c%d", 'A' + ams_id, slot + 1);
    else if (len) buf[0] = 0;
}

static int ev_prio(pet_state_t s) { return s == PET_FAIL ? 3 : s == PET_EMPTY ? 2 : s == PET_DONE ? 1 : 0; }

void pet_switch_init(pet_switch_t *w, int n) {
    memset(w, 0, sizeof *w);
    w->n = n < 1 ? 1 : (n > PET_MAX_PRINTERS ? PET_MAX_PRINTERS : n);
    w->pending = -1;
    for (int i = 0; i < PET_MAX_PRINTERS; i++) w->last[i] = PET_STATE_COUNT;   // 首次观察不算事件(开机时已在断料不乱跳)
}

static int pick_next(const pet_switch_t *w, const pet_state_t *st) {
    int best = -1, bp = 0;
    for (int i = 0; i < w->n; i++) {
        if (!(w->queued & (1u << i)) || i == w->cur) continue;
        int p = ev_prio(st[i]);
        if (p > bp) { bp = p; best = i; }
    }
    return best;
}

int pet_switch_tick(pet_switch_t *w, const pet_state_t *st, uint32_t now) {
    for (int i = 0; i < w->n; i++) {
        pet_state_t prev = w->last[i];
        w->last[i] = st[i];
        if (prev == PET_STATE_COUNT) continue;
        if (ev_prio(st[i]) == 0) { w->queued &= (uint8_t)~(1u << i); continue; }   // 事件已消失
        if (st[i] != prev && i != w->cur) { w->queued |= (uint8_t)(1u << i); w->ev_at[i] = now; }
    }
    w->queued &= (uint8_t)~(1u << w->cur);
    int best = pick_next(w, st);
    if (best < 0) { w->pending = -1; return -1; }
    if (w->pending < 0 || !(w->queued & (1u << w->pending))) {
        w->pending = best;
        uint32_t due = w->ev_at[best] + PET_JUMP_HOLD_MS;
        w->due = (int32_t)(w->next_ok - due) > 0 ? w->next_ok : due;
    } else if (best != w->pending && ev_prio(st[best]) > ev_prio(st[w->pending])) {
        w->pending = best;                 // 更严重的事件顶替,但不延长倒计时
    }
    if ((int32_t)(now - w->due) >= 0) return w->pending;
    return -1;
}

void pet_switch_set_cur(pet_switch_t *w, int idx, uint32_t now, bool manual) {
    if (idx < 0 || idx >= w->n) return;
    w->cur = idx;
    w->queued &= (uint8_t)~(1u << idx);
    w->pending = -1;
    w->next_ok = now + PET_JUMP_HOLD_MS;   // 手动或自动切换后,下一次自动跳转至少再等 3 秒
    (void)manual;
}

// ---- v4.3 耗材页 ----
void pet_ams_reset(pet_ams_t *a) {
    memset(a, 0, sizeof *a);
}
bool pet_ams_is_ht(int id) { return id >= 128 && id < 136; }
void pet_ams_unit_name(int id, char *buf, size_t len) {
    if (pet_ams_is_ht(id)) snprintf(buf, len, "HT-%c", 'A' + (id - 128));
    else if (id >= 0 && id < 4) snprintf(buf, len, "AMS-%c", 'A' + id);
    else snprintf(buf, len, "AMS-%d", id);
}
void pet_ams_humidity_text(const pet_ams_unit_t *u, char *buf, size_t len) {
    if (u->hum_pct >= 0) snprintf(buf, len, "%d%%", u->hum_pct);
    else if (u->hum_lvl >= 0) snprintf(buf, len, "%d级", u->hum_lvl);
    else if (len) buf[0] = 0;
}
bool pet_tray_dark(const pet_tray_t *t) {
    return pet_luma(((uint32_t)t->r << 16) | ((uint32_t)t->g << 8) | t->b) < 48;
}
bool pet_tray_low(const pet_tray_t *t) {
    return (t->flags & PET_TRAY_KNOWN) && !(t->flags & PET_TRAY_EMPTY) && t->remain >= 0 && t->remain <= 15;
}
void pet_ams_mark_active(pet_ams_t *a, const int *snow, int n_snow) {
    const uint8_t clr = PET_TRAY_ACTIVE | PET_TRAY_NOZ_L | PET_TRAY_NOZ_R;
    for (int i = 0; i < a->n_slots; i++) a->slot[i].flags &= ~clr;
    for (int i = 0; i < a->n_ext; i++) a->ext[i].flags &= ~clr;
    for (int k = 0; k < n_snow; k++) {
        if (snow[k] < 0) continue;
        uint8_t noz = n_snow == 2 ? (k == 0 ? PET_TRAY_NOZ_R : PET_TRAY_NOZ_L) : 0;
        int ams = (snow[k] >> 8) & 0xFF, slot = snow[k] & 0xFF;
        if ((ams == 254 || ams == 255) && slot == 0) {           // 外挂:254 = 左,255 = 右(单喷嘴机型只有一个)
            for (int i = 0; i < a->n_ext; i++) {
                uint8_t f = a->ext[i].flags;
                bool side_ok = !(f & (PET_TRAY_SIDE_L | PET_TRAY_SIDE_R)) ||
                               ((f & PET_TRAY_SIDE_L) && ams == 254) || ((f & PET_TRAY_SIDE_R) && ams == 255);
                if (side_ok) a->ext[i].flags |= PET_TRAY_ACTIVE | noz;
            }
            continue;
        }
        if (ams >= 254 || slot >= 4) continue;                   // 0xFFFF / 0x??FF = 没有料
        for (int u = 0; u < a->n_units; u++) {
            if (a->u[u].id != ams || slot >= a->u[u].nslot) continue;
            a->slot[a->u[u].first + slot].flags |= PET_TRAY_ACTIVE | noz;
        }
    }
}
const char *pet_tray_tag(const pet_tray_t *t) {
    if (!(t->flags & PET_TRAY_ACTIVE)) return "";
    bool l = t->flags & PET_TRAY_NOZ_L, r = t->flags & PET_TRAY_NOZ_R;
    return l && r ? "LR" : l ? "L" : r ? "R" : "";
}
int pet_ams_row_count(const pet_ams_t *a) { return a->n_units + (a->n_ext ? 1 : 0); }
int pet_ams_row_h(const pet_ams_t *a, int row) { return row < a->n_units ? PET_AMS_ROW_H : PET_AMS_EXT_H; }
int pet_ams_rows_fit(const pet_ams_t *a, int first, int view_h) {
    int n = pet_ams_row_count(a), used = 0, k = 0;
    for (int r = first; r < n; r++) {
        int h = pet_ams_row_h(a, r) + (k ? PET_AMS_GAP : 0);
        if (used + h > view_h) break;
        used += h; k++;
    }
    return k ? k : (first < n ? 1 : 0);
}
int pet_ams_scroll(const pet_ams_t *a, int first, int dir, int view_h) {
    int n = pet_ams_row_count(a);
    if (first < 0) first = 0;
    if (dir < 0) return first > 0 ? first - 1 : -1;
    if (first + pet_ams_rows_fit(a, first, view_h) >= n) return -1;   // 最后一行已经完整显示
    return first + 1;
}
static void fake_tray(pet_tray_t *t, uint32_t rgb, const char *type, int remain, uint8_t flags) {
    t->r = rgb >> 16; t->g = (rgb >> 8) & 0xFF; t->b = rgb & 0xFF;
    t->flags = PET_TRAY_KNOWN | flags; t->remain = (int8_t)remain;
    snprintf(t->type, sizeof t->type, "%s", type);
}
void pet_fake_ams(int printer, pet_ams_t *a) {
    pet_ams_reset(a);
    a->valid = true;
    if (printer == 0) {
        // 示例数据(按一台真实 H2C 的 Handy 截图整理,2026-10):AMS-A 39% 绿(左喷嘴在用) 粉 灰 米白;AMS-B 46% 黑 蓝 红(余量低) 空;HT-A 22% 白(右喷嘴在用)
        static const uint32_t C[8] = { 0x00AE42, 0xF5B6CD, 0x8E9089, 0xF4E4D6, 0x000000, 0x0A2CA5, 0xC12E1F, 0 };
        static const int8_t R[8] = { 80, 65, 40, 90, 70, 55, 10, -1 };
        a->n_units = 3; a->n_slots = 9; a->n_ext = 1;
        a->u[0] = (pet_ams_unit_t){ 0, 39, -1, 0, 4 };
        a->u[1] = (pet_ams_unit_t){ 1, 46, -1, 4, 4 };
        a->u[2] = (pet_ams_unit_t){ 128, 22, -1, 8, 1 };
        for (int i = 0; i < 8; i++) {
            if (i == 7) fake_tray(&a->slot[i], 0, "", -1, PET_TRAY_EMPTY);
            else fake_tray(&a->slot[i], C[i], "PLA", R[i], i == 0 ? PET_TRAY_ACTIVE | PET_TRAY_NOZ_L : 0);   // 左喷嘴 A1 绿
        }
        fake_tray(&a->slot[8], 0xFFFFFF, "PLA", 60, PET_TRAY_ACTIVE | PET_TRAY_NOZ_R);   // 右喷嘴 HT-A 白
        fake_tray(&a->ext[0], 0, "", -1, PET_TRAY_EMPTY | PET_TRAY_SIDE_L);       // 「左 未设置」
    } else {
        static const uint32_t C[4] = { 0xFFFFFF, 0x3C78E6, 0x161616, 0xFF8C3C };
        static const char *T[4] = { "PLA", "PETG", "PLA", "PLA" };
        a->n_units = 1; a->n_slots = 4; a->n_ext = 1;
        a->u[0] = (pet_ams_unit_t){ 0, -1, 4, 0, 4 };
        for (int i = 0; i < 4; i++) fake_tray(&a->slot[i], C[i], T[i], i == 3 ? 12 : 70, i == 1 ? PET_TRAY_ACTIVE : 0);
        fake_tray(&a->ext[0], 0, "", -1, PET_TRAY_EMPTY);
    }
}

// ---- v4.4 按键 ----
void pet_okkey_init(pet_okkey_t *k) { memset(k, 0, sizeof *k); }
pet_ok_ev_t pet_okkey_press(pet_okkey_t *k, uint32_t now) {
    pet_ok_ev_t ev = PET_OK_NONE;
    k->down = true; k->long_fired = false;
    if (k->pending) {
        if ((uint32_t)(now - k->t_up) <= PET_DBL_MS) k->second = true;
        else { k->pending = false; ev = PET_OK_SINGLE; }   // tick 没来得及补发:先补上一次单击
    }
    return ev;
}
void pet_okkey_long(pet_okkey_t *k) { k->long_fired = true; k->pending = false; k->second = false; }
pet_ok_ev_t pet_okkey_release(pet_okkey_t *k, uint32_t now, bool dbl_enabled) {
    if (!k->down) return PET_OK_NONE;
    k->down = false;
    if (k->long_fired) { k->long_fired = false; return PET_OK_NONE; }
    if (k->second) { k->pending = k->second = false; return PET_OK_DOUBLE; }
    if (!dbl_enabled) { k->pending = false; return PET_OK_SINGLE; }
    k->pending = true; k->t_up = now;
    return PET_OK_NONE;
}
pet_ok_ev_t pet_okkey_tick(pet_okkey_t *k, uint32_t now) {
    if (k->pending && !k->second && !k->down && (uint32_t)(now - k->t_up) > PET_DBL_MS) { k->pending = false; return PET_OK_SINGLE; }
    return PET_OK_NONE;
}
bool pet_dbl_enabled(bool demo, int n_printers) { return !demo && n_printers >= 2; }
int pet_page_step(int page, int dir) { return ((page + (dir > 0 ? 1 : -1)) % PET_PAGES + PET_PAGES) % PET_PAGES; }
const char *pet_hint_text(int page, bool demo, int n_printers, int step, char *buf, size_t len) {
    if (demo) snprintf(buf, len, "中键切状态 %d/6 · 上下键切页", step + 1);
    else snprintf(buf, len, "%s%s", page == 2 ? "上下键翻页/切页" : "上下键切页", n_printers >= 2 ? " · 双击中键换机器" : "");
    return buf;
}
bool pet_hint_on_page(int page) { return page == 1 || page == 2; }
bool pet_demo_badge_on_page(int page) { return page == 0; }
bool pet_dots_visible(int n_printers) { return n_printers >= 2; }
void pet_temp_pair(bool valid, float cur, float target, bool has_target, char *cbuf, size_t clen, char *tbuf, size_t tlen) {
    if (!valid) { snprintf(cbuf, clen, "--"); snprintf(tbuf, tlen, "%s", ""); return; }
    int c = (int)(cur + (cur >= 0 ? 0.5f : -0.5f)), t = (int)(target + (target >= 0 ? 0.5f : -0.5f));
    snprintf(cbuf, clen, "%d", c);
    if (has_target) snprintf(tbuf, tlen, "/%d°C", t < 0 ? 0 : t); else snprintf(tbuf, tlen, "/--");
}
bool pet_is_dual(int n_nozzles, const char *product) {
    return n_nozzles >= 2 || (product && (product[0] == 'H' || product[0] == 'h') && product[1] == '2');
}
void pet_conn_init(pet_conn_t *c) { memset(c, 0, sizeof *c); }
pet_conn_state_t pet_conn_update_ex(pet_conn_t *c, bool has_data, bool mqtt_up, bool parse_fail, uint32_t now_ms) {
    if (has_data) { c->had_data = true; c->started = false; return PET_CONN_DATA; }
    if (!c->started) { c->started = true; c->t0 = now_ms; }
    bool late = (uint32_t)(now_ms - c->t0) >= PET_CONN_FAIL_MS;
    if (mqtt_up && (parse_fail || late)) return PET_CONN_NODATA;
    return late ? PET_CONN_FAILED : PET_CONN_CONNECTING;
}
pet_conn_state_t pet_conn_update(pet_conn_t *c, bool has_data, uint32_t now_ms) {
    if (has_data) { c->had_data = true; c->started = false; return PET_CONN_DATA; }
    if (!c->started) { c->started = true; c->t0 = now_ms; }
    return (uint32_t)(now_ms - c->t0) >= PET_CONN_FAIL_MS ? PET_CONN_FAILED : PET_CONN_CONNECTING;
}

// v4.5.2 配网热点名(net.c 设 SSID、pet_ui.c 画二维码和步骤文字都调用这里)
void pet_ap_name(const uint8_t mac[6], char *out, size_t len) { snprintf(out, len, PET_AP_PREFIX "%02X%02X", mac[4], mac[5]); }
void pet_ap_qr(const char *ap, char *out, size_t len) { snprintf(out, len, "WIFI:T:nopass;S:%s;;", ap); }
void pet_ap_step(const char *ap, char *out, size_t len) { snprintf(out, len, "连上热点 %s", ap); }

// v4.5.3 恢复出厂防误触
void pet_reset_init(pet_reset_t *r) { r->st = PET_RST_OFF; r->t0 = 0; }
void pet_reset_open(pet_reset_t *r, bool ok_down) { r->st = ok_down ? PET_RST_WAIT_RELEASE : PET_RST_READY; r->t0 = 0; }
void pet_reset_close(pet_reset_t *r) { pet_reset_init(r); }
void pet_reset_press(pet_reset_t *r, uint32_t now) {
    if (r->st == PET_RST_READY) { r->st = PET_RST_HOLDING; r->t0 = now; }
    // WAIT_RELEASE:还没看到松手,这次按下不算(理论上不会发生,防驱动丢 RELEASE)
}
pet_rst_ev_t pet_reset_release(pet_reset_t *r, uint32_t now) {
    (void)now;
    if (r->st == PET_RST_WAIT_RELEASE) { r->st = PET_RST_READY; return PET_RST_EV_NONE; }
    if (r->st == PET_RST_HOLDING) { r->st = PET_RST_OFF; return PET_RST_EV_CANCEL; }
    return PET_RST_EV_NONE;
}
pet_rst_ev_t pet_reset_tick(pet_reset_t *r, uint32_t now) {
    if (r->st != PET_RST_HOLDING) return PET_RST_EV_NONE;
    if ((uint32_t)(now - r->t0) >= PET_RESET_HOLD_MS) { r->st = PET_RST_DONE; return PET_RST_EV_RESET; }
    return PET_RST_EV_NONE;
}
uint32_t pet_reset_elapsed(const pet_reset_t *r, uint32_t now) { return r->st == PET_RST_HOLDING ? (uint32_t)(now - r->t0) : 0; }
int pet_set_step(int sel, int dir, int count) {
    int n = sel + (dir > 0 ? 1 : -1);
    return n < 0 ? 0 : n >= count ? count - 1 : n;
}
