// main/wifi_roam.h — v4.8 多组 Wi-Fi 的选网与连接状态机。纯逻辑,不含 ESP-IDF 依赖,主机可测;
// net.c 只负责把状态机给出的动作(扫描 / 连接某一组 / 等待)翻译成 esp_wifi_* 调用并把事件喂回来。
//
// 流程(只在已存 >= 2 组时启用;只有 1 组时 net.c 走 v4.7.x 原来的单组路径,行为完全不变):
//   开机 / 断线退避到期 → 扫描 → 已存 SSID 与扫描结果匹配,按信号强到弱排序(同 SSID 多个 AP 取最强)→
//   逐个连接,每个最多等 ROAM_CONNECT_TIMEOUT_MS;认证失败/断开立刻换下一个 → 全部失败 → 退避(1→2→4…→60 s,与 v4.7.x 相同)后重新扫描。
//   不会自动进配网(v4.7.x 也不会)。
// 隐藏 SSID 不支持:扫描里看不到的 SSID 不会被尝试。
#pragma once
#include "wifi_slots.h"

// 每组等拿到 IP 的上限。15 s = 配网页等 IP 的现有值(setup_portal.c ensure_internet);5 组全部超时最坏 75 s(+扫描),之后进入退避。
#define ROAM_CONNECT_TIMEOUT_MS 15000
#define ROAM_BACKOFF_INIT_MS    1000
#define ROAM_BACKOFF_MAX_MS     60000

typedef struct {
    bool    seen[WIFI_SLOTS_MAX];
    int8_t  best[WIFI_SLOTS_MAX];    // 每组在扫描里看到的最强 RSSI
    uint8_t order[WIFI_SLOTS_MAX];   // 尝试顺序(槽位号)
    uint8_t n, pos;
} wifi_plan_t;

void wifi_plan_begin(wifi_plan_t *p);
void wifi_plan_feed(wifi_plan_t *p, const wifi_slots_t *w, const char *ssid, int rssi);   // 每条扫描记录调一次
void wifi_plan_finish(wifi_plan_t *p);   // 看到的按 RSSI 降序(同强度按槽位号);没看到的不进计划
void wifi_plan_all(wifi_plan_t *p, const wifi_slots_t *w);   // 不扫描时:按保存顺序全部尝试
int  wifi_plan_next(wifi_plan_t *p);     // 下一个槽位号;用完返回 -1

typedef enum { ROAM_IDLE = 0, ROAM_SCANNING, ROAM_TRYING, ROAM_ONLINE, ROAM_BACKOFF } roam_state_t;
typedef enum { ROAM_EV_START, ROAM_EV_SCAN_DONE, ROAM_EV_SCAN_FAIL, ROAM_EV_GOT_IP, ROAM_EV_DISCONNECTED, ROAM_EV_TIMER } roam_ev_t;
typedef enum { ROAM_ACT_NONE = 0, ROAM_ACT_SCAN, ROAM_ACT_CONNECT, ROAM_ACT_WAIT } roam_act_kind_t;
typedef struct { roam_act_kind_t kind; int slot; int ms; } roam_act_t;   // CONNECT:连 slot,ms 内没拿到 IP 就会收到 TIMER;WAIT:ms 后收到 TIMER

typedef struct {
    roam_state_t st;
    wifi_plan_t plan;
    int backoff_ms;
    bool no_scan;      // 配网热点开着时不扫描(AP+STA 同时扫描会让手机掉线):按保存顺序直接试
} roam_t;

void roam_init(roam_t *r);
// 喂事件,拿动作。SCAN_DONE 之前调用方必须已经对 r->plan 做完 begin/feed/finish。
roam_act_t roam_step(roam_t *r, roam_ev_t ev, const wifi_slots_t *w);

// ---- 配网页「添加 Wi-Fi」试连失败时,用 STA 断开原因码区分「密码不对」和「连不上」----
// 依据 ESP-IDF 文档 docs/en/api-guides/wifi.rst「Wi-Fi Reason code related to wrong password」:
//   WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT(15)、WIFI_REASON_HANDSHAKE_TIMEOUT(204)= 加密网络上口令不对。
//   (文档里 NO_AP_FOUND(201) 也可能是口令问题——给开放网络填了口令或给加密网络没填——但它更常见的原因是没搜到/太远,所以归「连不上」。)
// 数值写成常量而不 include esp_wifi_types.h,以便主机测试;net.c 里有 _Static_assert 保证和 ESP-IDF 一致。
#define WIFI_RSN_ASSOC_LEAVE          8     // 我们自己调 esp_wifi_disconnect()
#define WIFI_RSN_4WAY_HS_TIMEOUT      15
#define WIFI_RSN_HANDSHAKE_TIMEOUT    204
bool wifi_fail_is_password(int reason);

// ---- v4.8 必修:漫游自己发起的扫描被丢弃时,驱动里的扫描列表要清掉 ----
// 驱动里的 AP 列表只有 esp_wifi_scan_get_ap_record(s) 或 esp_wifi_clear_ap_list() 才释放。
// net.c 在漫游的 esp_wifi_scan_start(block=false) 成功后置 pending;收到 SCAN_DONE 时按下表处理:
//   pending=false            → IGNORE :不是我们的扫描(配网页 net_scan 的阻塞扫描),不碰,让配网页自己取
//   pending 且漫游开着、仍在 SCANNING → CONSUME:照常 roam_scan_done() 逐条取
//   pending 但漫游已停/状态已变      → CLEAR  :结果没人要,esp_wifi_clear_ap_list() 释放
typedef enum { ROAM_SCAN_IGNORE = 0, ROAM_SCAN_CONSUME, ROAM_SCAN_CLEAR } roam_scan_disp_t;
roam_scan_disp_t roam_scan_done_disposition(bool pending, bool roam_on, roam_state_t st);
