// main/wifi_slots.c — 见 wifi_slots.h
#include "wifi_slots.h"
#include <string.h>

static void put(char *dst, size_t cap, const char *src) {   // 不依赖 strlcpy,主机也能编
    size_t n = src ? strlen(src) : 0;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(dst, src, n);
    memset(dst + n, 0, cap - n);
}

void wifi_slots_init(wifi_slots_t *w) { memset(w, 0, sizeof *w); }

bool wifi_slots_valid(const char *ssid, const char *pass) {
    if (!ssid) return false;
    size_t a = strlen(ssid), b = pass ? strlen(pass) : 0;
    return a >= 1 && a <= WIFI_SSID_MAX && b <= WIFI_PASS_MAX;
}

bool wifi_pass_ok(const char *pass) {
    size_t n = pass ? strlen(pass) : 0;
    if (n == 0) return true;
    if (n >= 8 && n <= 63) return true;
    if (n != 64) return false;
    for (size_t i = 0; i < n; i++) {
        char c = pass[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

int wifi_slots_find(const wifi_slots_t *w, const char *ssid) {
    if (!ssid) return -1;
    for (int i = 0; i < w->n; i++) if (strcmp(w->s[i].ssid, ssid) == 0) return i;
    return -1;
}

wifi_res_t wifi_slots_add(wifi_slots_t *w, const char *ssid, const char *pass) {
    if (!wifi_slots_valid(ssid, pass)) return WIFI_E_INVALID;
    int i = wifi_slots_find(w, ssid);
    if (i < 0) {
        if (w->n >= WIFI_SLOTS_MAX) return WIFI_E_FULL;
        i = w->n++;
        put(w->s[i].ssid, sizeof w->s[i].ssid, ssid);
    }
    put(w->s[i].pass, sizeof w->s[i].pass, pass);
    return WIFI_OK;
}

wifi_res_t wifi_slots_delete(wifi_slots_t *w, const char *ssid) {
    int i = wifi_slots_find(w, ssid);
    if (i < 0) return WIFI_E_NOTFOUND;
    if (w->n <= 1) return WIFI_E_LAST;
    memmove(&w->s[i], &w->s[i + 1], (size_t)(w->n - 1 - i) * sizeof w->s[0]);
    w->n--;
    memset(&w->s[w->n], 0, sizeof w->s[0]);
    return WIFI_OK;
}

bool wifi_slots_promote(wifi_slots_t *w, const char *ssid, const char *pass) {
    if (!wifi_slots_valid(ssid, pass)) return false;
    int i = wifi_slots_find(w, ssid);
    if (i == 0 && strcmp(w->s[0].pass, pass ? pass : "") == 0) return false;
    if (i < 0) {
        if (w->n >= WIFI_SLOTS_MAX) w->n = WIFI_SLOTS_MAX - 1;   // 挤掉最后一组(最久没用的)
        i = w->n;
        memset(&w->s[i], 0, sizeof w->s[i]);
        w->n++;
    }
    wifi_slot_t t;
    put(t.ssid, sizeof t.ssid, ssid);
    put(t.pass, sizeof t.pass, pass);
    memmove(&w->s[1], &w->s[0], (size_t)i * sizeof w->s[0]);
    w->s[0] = t;
    return true;
}

bool wifi_slots_reconcile(wifi_slots_t *w, const char *legacy_ssid, const char *legacy_pass) {
    if (!legacy_ssid || !legacy_ssid[0]) return false;
    return wifi_slots_promote(w, legacy_ssid, legacy_pass);
}

size_t wifi_nvs_blob_bytes(int n) { return n < 0 ? 0 : (size_t)2 + (size_t)n * WIFI_SLOT_BYTES; }
int wifi_nvs_blob_entries(int n) { return 1 + (int)((wifi_nvs_blob_bytes(n) + 31) / 32) + 1; }
int wifi_nvs_str_entries(size_t len) { return 1 + (int)((len + 1 + 31) / 32); }

size_t wifi_slots_pack(const wifi_slots_t *w, uint8_t *buf, size_t cap) {
    if (w->n < 0 || w->n > WIFI_SLOTS_MAX) return 0;
    size_t need = wifi_nvs_blob_bytes(w->n);
    if (cap < need) return 0;
    buf[0] = WIFI_BLOB_VER;
    buf[1] = (uint8_t)w->n;
    for (int i = 0; i < w->n; i++) {
        uint8_t *p = buf + 2 + (size_t)i * WIFI_SLOT_BYTES;
        put((char *)p, WIFI_SSID_MAX + 1, w->s[i].ssid);
        put((char *)p + WIFI_SSID_MAX + 1, WIFI_PASS_MAX + 1, w->s[i].pass);
    }
    return need;
}

bool wifi_slots_unpack(wifi_slots_t *w, const uint8_t *buf, size_t len) {
    wifi_slots_init(w);
    if (!buf || len < 2 || buf[0] != WIFI_BLOB_VER || buf[1] > WIFI_SLOTS_MAX) return false;
    int n = buf[1];
    if (len != wifi_nvs_blob_bytes(n)) return false;
    for (int i = 0; i < n; i++) {
        const uint8_t *p = buf + 2 + (size_t)i * WIFI_SLOT_BYTES;
        if (p[WIFI_SSID_MAX] != 0 || p[WIFI_SSID_MAX + 1 + WIFI_PASS_MAX] != 0 || p[0] == 0) { wifi_slots_init(w); return false; }
        memcpy(w->s[i].ssid, p, WIFI_SSID_MAX + 1);
        memcpy(w->s[i].pass, p + WIFI_SSID_MAX + 1, WIFI_PASS_MAX + 1);
        for (int j = 0; j < i; j++) if (strcmp(w->s[j].ssid, w->s[i].ssid) == 0) { wifi_slots_init(w); return false; }   // 重名视为损坏
    }
    w->n = n;
    return true;
}
