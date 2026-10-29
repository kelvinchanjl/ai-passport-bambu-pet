// v4.8 多组 Wi-Fi 的选网与连接状态机:扫描匹配排序、逐个尝试、15 s 超时、全部失败后的退避、配网热点开着时不扫描。纯 C。
#include "wifi_roam.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static wifi_slots_t W;
static roam_t R;
static void slots3(void) { wifi_slots_init(&W); wifi_slots_add(&W, "A", "pa"); wifi_slots_add(&W, "B", "pb"); wifi_slots_add(&W, "C", "pc"); }
static roam_act_t ev(roam_ev_t e) { return roam_step(&R, e, &W); }
// 模拟一次扫描结果然后喂 SCAN_DONE
static roam_act_t scan_done(const char *const *ssids, const int *rssi, int n) {
    wifi_plan_begin(&R.plan);
    for (int i = 0; i < n; i++) wifi_plan_feed(&R.plan, &W, ssids[i], rssi[i]);
    wifi_plan_finish(&R.plan);
    return ev(ROAM_EV_SCAN_DONE);
}
#define IS(a, k, s, m) assert((a).kind == (k) && (a).slot == (s) && (a).ms == (m))

int main(void) {
    assert(ROAM_CONNECT_TIMEOUT_MS == 15000 && ROAM_BACKOFF_INIT_MS == 1000 && ROAM_BACKOFF_MAX_MS == 60000);

    // ---- 计划:只含扫描里看到的已存 SSID,按信号强弱,同名多 AP 取最强 ----
    slots3();
    wifi_plan_t p; wifi_plan_begin(&p);
    wifi_plan_feed(&p, &W, "C", -80); wifi_plan_feed(&p, &W, "Other", -30); wifi_plan_feed(&p, &W, "A", -70);
    wifi_plan_feed(&p, &W, "C", -55);          // 同 SSID 的另一个更强 AP
    wifi_plan_feed(&p, &W, "C", -90);          // 更弱的忽略
    wifi_plan_finish(&p);
    assert(p.n == 2 && wifi_plan_next(&p) == 2 && wifi_plan_next(&p) == 0 && wifi_plan_next(&p) == -1 && wifi_plan_next(&p) == -1);   // C(-55) 先于 A(-70);B 没扫到 → 不试
    wifi_plan_begin(&p); wifi_plan_feed(&p, &W, "B", -60); wifi_plan_feed(&p, &W, "A", -60); wifi_plan_finish(&p);
    assert(wifi_plan_next(&p) == 0 && wifi_plan_next(&p) == 1);             // 同强度按保存顺序
    wifi_plan_begin(&p); wifi_plan_feed(&p, &W, "X", -10); wifi_plan_finish(&p); assert(p.n == 0 && wifi_plan_next(&p) == -1);   // 一个都没匹配
    wifi_plan_begin(&p); wifi_plan_feed(&p, &W, "A", 0); wifi_plan_feed(&p, &W, "B", -128); wifi_plan_feed(&p, &W, "C", 127); wifi_plan_finish(&p);
    assert(wifi_plan_next(&p) == 2 && wifi_plan_next(&p) == 0 && wifi_plan_next(&p) == 1);   // 边界 RSSI 值
    wifi_plan_all(&p, &W); assert(p.n == 3 && wifi_plan_next(&p) == 0 && wifi_plan_next(&p) == 1 && wifi_plan_next(&p) == 2);
    wifi_slots_init(&W); wifi_plan_begin(&p); wifi_plan_feed(&p, &W, "A", -50); wifi_plan_finish(&p); assert(p.n == 0);   // 空列表

    // ---- 场景 1:开机 → 扫描 → 最强的先连,超时换下一个,连上后掉线再来一轮 ----
    slots3(); roam_init(&R);
    roam_act_t a = ev(ROAM_EV_START); IS(a, ROAM_ACT_SCAN, -1, 0); assert(R.st == ROAM_SCANNING);
    { const char *s[] = { "A", "B", "C" }; int r[] = { -70, -50, -60 };
      a = scan_done(s, r, 3); IS(a, ROAM_ACT_CONNECT, 1, 15000); assert(R.st == ROAM_TRYING); }   // B 最强
    a = ev(ROAM_EV_TIMER); IS(a, ROAM_ACT_CONNECT, 2, 15000);                                   // B 超时 → C
    a = ev(ROAM_EV_DISCONNECTED); IS(a, ROAM_ACT_CONNECT, 0, 15000);                            // C 认证失败(不等超时)→ A
    a = ev(ROAM_EV_GOT_IP); assert(a.kind == ROAM_ACT_NONE && R.st == ROAM_ONLINE && R.backoff_ms == 1000);
    a = ev(ROAM_EV_TIMER); assert(a.kind == ROAM_ACT_NONE && R.st == ROAM_ONLINE);              // 在线时的残留定时器不生效
    a = ev(ROAM_EV_DISCONNECTED); IS(a, ROAM_ACT_WAIT, -1, 1000); assert(R.st == ROAM_BACKOFF);  // 掉线:1 s 后重扫,与 v4.7.x 的 1 s 重连一致
    a = ev(ROAM_EV_TIMER); IS(a, ROAM_ACT_SCAN, -1, 0);

    // ---- 场景 2:全部失败 → 退避 1,2,4,…,60 s 封顶(与 v4.7.x 相同),每次退避结束重新扫描;连上后退避归位 ----
    slots3(); roam_init(&R);
    ev(ROAM_EV_START);
    { const char *s[] = { "A", "B", "C" }; int r[] = { -40, -50, -60 };
      int expect[] = { 1000, 2000, 4000, 8000, 16000, 32000, 60000, 60000, 60000 };
      for (int round = 0; round < 9; round++) {
          a = scan_done(s, r, 3); IS(a, ROAM_ACT_CONNECT, 0, 15000);
          a = ev(ROAM_EV_TIMER); IS(a, ROAM_ACT_CONNECT, 1, 15000);
          a = ev(ROAM_EV_DISCONNECTED); IS(a, ROAM_ACT_CONNECT, 2, 15000);
          a = ev(ROAM_EV_TIMER); IS(a, ROAM_ACT_WAIT, -1, expect[round]);
          assert(ev(ROAM_EV_DISCONNECTED).kind == ROAM_ACT_NONE && ev(ROAM_EV_SCAN_DONE).kind == ROAM_ACT_NONE);   // 退避期间的杂散事件被忽略
          a = ev(ROAM_EV_TIMER); IS(a, ROAM_ACT_SCAN, -1, 0);
      }
      a = scan_done(s, r, 3); IS(a, ROAM_ACT_CONNECT, 0, 15000);
      ev(ROAM_EV_GOT_IP); assert(R.backoff_ms == 1000);
      ev(ROAM_EV_DISCONNECTED); a = ev(ROAM_EV_TIMER); IS(a, ROAM_ACT_SCAN, -1, 0); }
    // 一轮全部超时的最坏耗时:5 组 × 15 s = 75 s(不含扫描),之后才进入退避
    assert(WIFI_SLOTS_MAX * ROAM_CONNECT_TIMEOUT_MS == 75000);

    // ---- 场景 3:扫描里一个已存网络都没有 / 扫描本身失败 → 直接退避,不会乱连,也不会进配网(状态机里没有这个动作)----
    slots3(); roam_init(&R); ev(ROAM_EV_START);
    { const char *s[] = { "Neighbour" }; int r[] = { -30 };
      a = scan_done(s, r, 1); IS(a, ROAM_ACT_WAIT, -1, 1000); }
    a = ev(ROAM_EV_TIMER); IS(a, ROAM_ACT_SCAN, -1, 0);
    a = ev(ROAM_EV_SCAN_FAIL); IS(a, ROAM_ACT_WAIT, -1, 2000);
    a = ev(ROAM_EV_SCAN_FAIL); assert(a.kind == ROAM_ACT_NONE);               // 不在扫描态的 SCAN_FAIL 忽略

    // ---- 场景 4:只有一个可见的网络 → 连它;它失败后退避 ----
    slots3(); roam_init(&R); ev(ROAM_EV_START);
    { const char *s[] = { "C" }; int r[] = { -75 };
      a = scan_done(s, r, 1); IS(a, ROAM_ACT_CONNECT, 2, 15000); }
    a = ev(ROAM_EV_TIMER); IS(a, ROAM_ACT_WAIT, -1, 1000);

    // ---- 场景 5:配网热点开着(no_scan):不扫描,按保存顺序直接试;全部失败后退避,到期再从第 0 组开始 ----
    slots3(); roam_init(&R); R.no_scan = true;
    a = ev(ROAM_EV_START); IS(a, ROAM_ACT_CONNECT, 0, 15000);
    a = ev(ROAM_EV_DISCONNECTED); IS(a, ROAM_ACT_CONNECT, 1, 15000);
    a = ev(ROAM_EV_TIMER); IS(a, ROAM_ACT_CONNECT, 2, 15000);
    a = ev(ROAM_EV_TIMER); IS(a, ROAM_ACT_WAIT, -1, 1000);
    a = ev(ROAM_EV_TIMER); IS(a, ROAM_ACT_CONNECT, 0, 15000);
    // 中途热点关了(no_scan 变 false)→ 下一轮退避到期就走扫描
    ev(ROAM_EV_TIMER); ev(ROAM_EV_TIMER); a = ev(ROAM_EV_TIMER); assert(a.kind == ROAM_ACT_WAIT);
    R.no_scan = false; a = ev(ROAM_EV_TIMER); IS(a, ROAM_ACT_SCAN, -1, 0);

    // ---- 场景 6:重新 START(例如设置里重新配网后)会清退避、重新开始 ----
    slots3(); roam_init(&R); R.backoff_ms = 32000; R.st = ROAM_BACKOFF;
    a = ev(ROAM_EV_START); IS(a, ROAM_ACT_SCAN, -1, 0); assert(R.backoff_ms == 1000);

    // ---- 初始态收到杂散事件:不动 ----
    roam_init(&R);
    assert(ev(ROAM_EV_TIMER).kind == ROAM_ACT_NONE && ev(ROAM_EV_DISCONNECTED).kind == ROAM_ACT_NONE && ev(ROAM_EV_SCAN_DONE).kind == ROAM_ACT_NONE && R.st == ROAM_IDLE);

    printf("test_wifi_roam_v48 OK\n");
    return 0;
}
