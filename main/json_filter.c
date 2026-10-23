// main/json_filter.c — 见 json_filter.h
#include "json_filter.h"
#include <string.h>

const char *const JF_BAMBU_KEYS[] = {
    "print", "gcode_state", "mc_percent", "mc_remaining_time", "layer_num", "total_layer_num",
    "nozzle_temper", "nozzle_target_temper", "bed_temper", "bed_target_temper", "chamber_temper",
    "print_error", "subtask_id", "task_id", "subtask_name",
    "device", "extruder", "state", "info", "id", "temp", "snow", "bed", "ctc",
    "ams", "tray_now", "tray_exist_bits", "humidity", "humidity_raw", "tray", "tray_color", "tray_type", "remain",
    "vt_tray", "vir_slot", "hms", "attr", "code",
};
const size_t JF_BAMBU_N_KEYS = sizeof JF_BAMBU_KEYS / sizeof JF_BAMBU_KEYS[0];

enum { S_VALUE, S_STR, S_SCALAR, S_AFTER, S_KEY_OR_END, S_KEY, S_COLON, S_KEY_FIRST_OR_END_ARR };

void jf_init(jf_t *f, char *out, size_t cap, const char *const *keep, size_t n_keep) {
    memset(f, 0, sizeof *f);
    f->out = out; f->cap = cap; f->keep = keep; f->n_keep = n_keep; f->st = S_VALUE;
}
static void put(jf_t *f, char c) {
    if (f->len + 1 >= f->cap) { f->err = true; return; }
    f->out[f->len++] = c;
}
static void puts_(jf_t *f, const char *s, size_t n) { for (size_t i = 0; i < n; i++) put(f, s[i]); }
static bool skipping(const jf_t *f) { return f->skip_val || (f->depth && f->fr[f->depth - 1].skip); }
static bool kept(const jf_t *f) {
    if (f->key_overflow) return false;
    for (size_t i = 0; i < f->n_keep; i++)
        if (strlen(f->keep[i]) == f->key_len && !memcmp(f->keep[i], f->key, f->key_len)) return true;
    return false;
}
// 一个值开始前:数组元素要补逗号
static void value_start(jf_t *f) {
    if (skipping(f) || !f->depth) return;
    if (f->fr[f->depth - 1].arr) { if (f->fr[f->depth - 1].n++) put(f, ','); }
}
static void value_end(jf_t *f) {
    f->skip_val = false;
    if (!f->depth) { f->done = true; }
    f->st = S_AFTER;
}
static bool ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

void jf_feed(jf_t *f, const char *data, size_t len) {
    f->in_total += len;
    for (size_t i = 0; i < len && !f->err; i++) {
        char c = data[i];
    again:
        switch (f->st) {
        case S_VALUE:
            if (ws(c)) break;
            if (f->done) { f->err = true; break; }
            value_start(f);
            if (c == '{' || c == '[') {
                if (f->depth >= JF_MAX_DEPTH) { f->err = true; break; }
                bool sk = skipping(f);
                f->fr[f->depth].arr = c == '['; f->fr[f->depth].skip = sk; f->fr[f->depth].n = 0;
                f->depth++; f->skip_val = false;
                if (!sk) put(f, c);
                f->st = c == '{' ? S_KEY_OR_END : S_KEY_FIRST_OR_END_ARR;
            } else if (c == '"') {
                if (!skipping(f)) put(f, c);
                f->esc = 0; f->st = S_STR;
            } else if (c == '-' || (c >= '0' && c <= '9') || c == 't' || c == 'f' || c == 'n') {
                if (!skipping(f)) put(f, c);
                f->st = S_SCALAR;
            } else f->err = true;
            break;
        case S_KEY_FIRST_OR_END_ARR:
            if (ws(c)) break;
            if (c == ']') goto close;
            f->st = S_VALUE; goto again;
        case S_STR:
            if (!skipping(f)) put(f, c);
            if (f->esc) f->esc = 0;
            else if (c == '\\') f->esc = 1;
            else if (c == '"') value_end(f);
            else if ((unsigned char)c < 0x20) f->err = true;
            break;
        case S_SCALAR:
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '.' || c == '-' || c == '+' || c == 'E') {
                if (!skipping(f)) put(f, c);
                break;
            }
            value_end(f); goto again;
        case S_AFTER:
            if (ws(c)) break;
            if (!f->depth) { f->err = true; break; }
            if (c == ',') { f->st = f->fr[f->depth - 1].arr ? S_VALUE : S_KEY_OR_END; break; }
            if (c == '}' || c == ']') goto close;
            f->err = true; break;
        case S_KEY_OR_END:
            if (ws(c)) break;
            if (c == '}') goto close;
            if (c != '"') { f->err = true; break; }
            f->key_len = 0; f->key_overflow = false; f->esc = 0; f->st = S_KEY;
            break;
        case S_KEY:
            if (f->esc) { f->esc = 0; f->key_overflow = true; break; }   // 带转义的键名一律不认
            if (c == '\\') { f->esc = 1; break; }
            if (c == '"') { f->st = S_COLON; break; }
            if (f->key_len < JF_KEY_MAX) f->key[f->key_len++] = c; else f->key_overflow = true;
            break;
        case S_COLON:
            if (ws(c)) break;
            if (c != ':') { f->err = true; break; }
            if (!skipping(f) && kept(f)) {
                if (f->fr[f->depth - 1].n++) put(f, ',');
                put(f, '"'); puts_(f, f->key, f->key_len); put(f, '"'); put(f, ':');
                f->skip_val = false;
            } else f->skip_val = true;
            f->st = S_VALUE;
            break;
        }
        continue;
    close: {
            uint8_t want_arr = c == ']';
            if (!f->depth || f->fr[f->depth - 1].arr != want_arr) { f->err = true; continue; }
            bool sk = f->fr[f->depth - 1].skip;
            f->depth--;
            if (!sk) put(f, c);
            value_end(f);
        }
    }
}
bool jf_finish(jf_t *f) {
    if (!f->err && f->st == S_SCALAR && !f->depth) value_end(f);
    if (f->err || !f->done) { f->err = true; if (f->cap) f->out[f->len < f->cap ? f->len : f->cap - 1] = 0; return false; }
    f->out[f->len] = 0;
    return true;
}
