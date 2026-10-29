// main/wifi_roam.c — 见 wifi_roam.h
#include "wifi_roam.h"
#include <string.h>

void wifi_plan_begin(wifi_plan_t *p) { memset(p, 0, sizeof *p); }

void wifi_plan_feed(wifi_plan_t *p, const wifi_slots_t *w, const char *ssid, int rssi) {
    int i = wifi_slots_find(w, ssid);
    if (i < 0) return;
    if (rssi > 127) rssi = 127;
    if (rssi < -128) rssi = -128;
    if (!p->seen[i] || rssi > p->best[i]) { p->seen[i] = true; p->best[i] = (int8_t)rssi; }
}

void wifi_plan_finish(wifi_plan_t *p) {
    p->n = 0; p->pos = 0;
    for (int i = 0; i < WIFI_SLOTS_MAX; i++) if (p->seen[i]) p->order[p->n++] = (uint8_t)i;
    for (int a = 1; a < p->n; a++) {                      // 插入排序:最多 5 个
        uint8_t v = p->order[a]; int b = a - 1;
        while (b >= 0 && (p->best[p->order[b]] < p->best[v])) { p->order[b + 1] = p->order[b]; b--; }
        p->order[b + 1] = v;
    }
}

void wifi_plan_all(wifi_plan_t *p, const wifi_slots_t *w) {
    wifi_plan_begin(p);
    for (int i = 0; i < w->n && i < WIFI_SLOTS_MAX; i++) p->order[p->n++] = (uint8_t)i;
}

int wifi_plan_next(wifi_plan_t *p) { return p->pos < p->n ? p->order[p->pos++] : -1; }

void roam_init(roam_t *r) { memset(r, 0, sizeof *r); r->st = ROAM_IDLE; r->backoff_ms = ROAM_BACKOFF_INIT_MS; }

static roam_act_t act(roam_act_kind_t k, int slot, int ms) { roam_act_t a = { k, slot, ms }; return a; }

static roam_act_t wait_backoff(roam_t *r) {
    int ms = r->backoff_ms;
    r->backoff_ms = r->backoff_ms * 2 > ROAM_BACKOFF_MAX_MS ? ROAM_BACKOFF_MAX_MS : r->backoff_ms * 2;
    r->st = ROAM_BACKOFF;
    return act(ROAM_ACT_WAIT, -1, ms);
}

static roam_act_t try_next(roam_t *r) {
    int s = wifi_plan_next(&r->plan);
    if (s < 0) return wait_backoff(r);
    r->st = ROAM_TRYING;
    return act(ROAM_ACT_CONNECT, s, ROAM_CONNECT_TIMEOUT_MS);
}

static roam_act_t begin_round(roam_t *r, const wifi_slots_t *w) {
    if (r->no_scan) { wifi_plan_all(&r->plan, w); return try_next(r); }
    r->st = ROAM_SCANNING;
    return act(ROAM_ACT_SCAN, -1, 0);
}

roam_act_t roam_step(roam_t *r, roam_ev_t ev, const wifi_slots_t *w) {
    switch (ev) {
    case ROAM_EV_START:
        r->backoff_ms = ROAM_BACKOFF_INIT_MS;
        return begin_round(r, w);
    case ROAM_EV_SCAN_DONE:
        if (r->st != ROAM_SCANNING) return act(ROAM_ACT_NONE, -1, 0);
        return try_next(r);
    case ROAM_EV_SCAN_FAIL:
        if (r->st != ROAM_SCANNING) return act(ROAM_ACT_NONE, -1, 0);
        return wait_backoff(r);
    case ROAM_EV_GOT_IP:
        r->st = ROAM_ONLINE;
        r->backoff_ms = ROAM_BACKOFF_INIT_MS;
        return act(ROAM_ACT_NONE, -1, 0);
    case ROAM_EV_DISCONNECTED:
        if (r->st == ROAM_ONLINE) return wait_backoff(r);     // 在线掉线:1 s 后重扫(v4.7.x 也是 1 s 后重连)
        if (r->st == ROAM_TRYING) return try_next(r);         // 认证失败/被踢:不用等超时,直接换下一个
        return act(ROAM_ACT_NONE, -1, 0);
    case ROAM_EV_TIMER:
        if (r->st == ROAM_TRYING) return try_next(r);         // 这一组 15 s 没拿到 IP
        if (r->st == ROAM_BACKOFF) return begin_round(r, w);
        return act(ROAM_ACT_NONE, -1, 0);
    }
    return act(ROAM_ACT_NONE, -1, 0);
}

bool wifi_fail_is_password(int reason) { return reason == WIFI_RSN_4WAY_HS_TIMEOUT || reason == WIFI_RSN_HANDSHAKE_TIMEOUT; }
