// main/net.c
#include "net.h"
#include "pet_logic.h"
#include "sdkconfig.h"
#include "wifi_roam.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "net";
static EventGroupHandle_t s_eg;
#define BIT_IP BIT0
#define BIT_FAIL BIT1          // v4.8 net_try_connect:试连期间收到了(非自己触发的)断开事件
_Static_assert(WIFI_REASON_ASSOC_LEAVE == WIFI_RSN_ASSOC_LEAVE && WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT == WIFI_RSN_4WAY_HS_TIMEOUT && WIFI_REASON_HANDSHAKE_TIMEOUT == WIFI_RSN_HANDSHAKE_TIMEOUT, "wifi_roam.h 里的原因码常量必须和 ESP-IDF 一致");
static bool s_inited, s_ap_on, s_want_sta;
static esp_timer_handle_t s_retry;
static int s_backoff_ms = 1000;
static net_ip_cb_t s_cb;
static bool s_scanning;
static volatile bool s_try_on;       // net_try_connect 进行中
static volatile int s_try_reason;

// ---- v4.8 多组 Wi-Fi 漫游(选网/超时/退避逻辑在 wifi_roam.c,主机测试;这里只把动作翻译成 esp_wifi_* 调用)----
// 只在已存 >= 2 组时启用;只有 1 组时仍走下面 v4.7.x 的单组路径(net_sta_connect),行为不变。
// 全部在 esp 事件循环线程/esp_timer 线程里非阻塞完成(扫描用 esp_wifi_scan_start(block=false)),没有新任务,没有新栈。
static bool s_roam_on;
static wifi_slots_t *s_slots;      // 堆上一份(<= 494 B),仅启用漫游时才分配
static roam_t s_roam;
static SemaphoreHandle_t s_roam_mu;
static bool s_roam_scan_pending;   // v4.8 必修:漫游自己的非阻塞扫描已开始、SCAN_DONE 还没处理(在 s_roam_mu 内读写)
#if CONFIG_PET_LOG_REPORT_KEYS     // 测试版:在关键点打堆信息,真机上核对多组 Wi-Fi 没有拉低 min_heap
#define ROAM_HEAP(tag) ESP_LOGI(TAG, "heap@%s free=%u min=%u", tag, (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size())
#else
#define ROAM_HEAP(tag) ((void)0)
#endif

void net_set_ip_callback(net_ip_cb_t cb) { s_cb = cb; }

static void roam_exec(roam_act_t a);   // 调用方持有 s_roam_mu
static void roam_feed(roam_ev_t e) {   // 调用方持有 s_roam_mu
    s_roam.no_scan = s_ap_on;
    roam_exec(roam_step(&s_roam, e, s_slots));
}
static void roam_exec(roam_act_t a) {
    switch (a.kind) {
    case ROAM_ACT_SCAN: {
        esp_timer_stop(s_retry);
        esp_wifi_disconnect();
        wifi_scan_config_t sc = { .show_hidden = false };   // 隐藏 SSID 不支持
        s_scanning = true;
        ROAM_HEAP("scan_start");
        if (esp_wifi_scan_start(&sc, false) != ESP_OK) { s_scanning = false; ESP_LOGW(TAG, "roam: scan_start failed"); roam_feed(ROAM_EV_SCAN_FAIL); }
        else s_roam_scan_pending = true;   // 失败时不清:可能是上一轮我们自己的扫描还没结束,那次的 SCAN_DONE 仍要清
        break;
    }
    case ROAM_ACT_CONNECT: {
        wifi_config_t w = { 0 };
        const wifi_slot_t *sl = &s_slots->s[a.slot];
        strlcpy((char *)w.sta.ssid, sl->ssid, sizeof w.sta.ssid);
        strlcpy((char *)w.sta.password, sl->pass, sizeof w.sta.password);
        w.sta.threshold.authmode = sl->pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
        ESP_LOGI(TAG, "roam: try slot %d/%d (timeout %d ms)", a.slot, s_slots->n, a.ms);   // 日志里不打 SSID
        esp_timer_stop(s_retry);
        esp_wifi_disconnect();
        esp_wifi_set_config(WIFI_IF_STA, &w);
        esp_timer_start_once(s_retry, (uint64_t)a.ms * 1000);
        if (esp_wifi_connect() != ESP_OK) { ESP_LOGW(TAG, "roam: connect call failed"); roam_feed(ROAM_EV_DISCONNECTED); }
        break;
    }
    case ROAM_ACT_WAIT:
        ESP_LOGI(TAG, "roam: wait %d ms then rescan", a.ms);
        esp_timer_stop(s_retry);
        esp_timer_start_once(s_retry, (uint64_t)a.ms * 1000);
        break;
    case ROAM_ACT_NONE: break;
    }
}
static void roam_scan_done(void) {
    wifi_plan_begin(&s_roam.plan);
    wifi_ap_record_t rec;
    int seen = 0;
    while (esp_wifi_scan_get_ap_record(&rec) == ESP_OK) {   // 逐条取,栈上只有一条记录;取完驱动自己释放扫描列表
        wifi_plan_feed(&s_roam.plan, s_slots, (const char *)rec.ssid, rec.rssi);
        seen++;
    }
    wifi_plan_finish(&s_roam.plan);
    ESP_LOGI(TAG, "roam: scan %d APs, %d saved networks in range", seen, (int)s_roam.plan.n);
    ROAM_HEAP("scan_done");
    roam_feed(ROAM_EV_SCAN_DONE);
}

static void retry_cb(void *arg) {
    (void)arg;
    if (s_roam_on) {
        if (xSemaphoreTake(s_roam_mu, portMAX_DELAY) == pdTRUE) {
            if (s_roam_on) roam_feed(ROAM_EV_TIMER);
            xSemaphoreGive(s_roam_mu);
        }
        return;
    }
    if (s_want_sta && !s_scanning) esp_wifi_connect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)data;
    // v4.8 必修:SCAN_DONE 不再只在 s_roam_on 时处理。漫游停了(配网页试连/绑定)或重启了(配网页增删)以后,
    // 我们自己那次扫描的结果没人取,会一直留在驱动里。判定见 wifi_roam.h roam_scan_done_disposition。
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        if (s_roam_mu && xSemaphoreTake(s_roam_mu, portMAX_DELAY) == pdTRUE) {
            roam_scan_disp_t d = roam_scan_done_disposition(s_roam_scan_pending, s_roam_on, s_roam.st);
            if (s_roam_scan_pending) { s_roam_scan_pending = false; s_scanning = false; }
            if (d == ROAM_SCAN_CONSUME) roam_scan_done();
            else if (d == ROAM_SCAN_CLEAR) { esp_wifi_clear_ap_list(); ESP_LOGI(TAG, "roam: stale scan result cleared"); ROAM_HEAP("scan_clear"); }
            xSemaphoreGive(s_roam_mu);
        }
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_want_sta && !s_roam_on) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        bool had = xEventGroupGetBits(s_eg) & BIT_IP;
        xEventGroupClearBits(s_eg, BIT_IP);
        if (had && s_cb) s_cb(false);
        if (s_try_on) {   // 配网页试连:自己调 esp_wifi_disconnect() 带 ASSOC_LEAVE,不算失败;其余原因码记下来
            const wifi_event_sta_disconnected_t *td = data;
            if (td && td->reason != WIFI_REASON_ASSOC_LEAVE) { s_try_reason = td->reason; xEventGroupSetBits(s_eg, BIT_FAIL); }
        }
        if (s_roam_on) {
            // 我们自己调 esp_wifi_disconnect()(换下一组/开始扫描)会带 ASSOC_LEAVE,不是这一组失败,忽略
            // 在线(ONLINE)时我们从不主动断开,所以那时的 ASSOC_LEAVE 是路由器踢的,必须处理,否则会永远掉线。
            const wifi_event_sta_disconnected_t *d = data;
            if (xSemaphoreTake(s_roam_mu, portMAX_DELAY) == pdTRUE) {
                bool own = d && d->reason == WIFI_REASON_ASSOC_LEAVE && s_roam.st != ROAM_ONLINE;
                if (s_roam_on && !own) { ESP_LOGI(TAG, "roam: disconnected reason=%d", d ? d->reason : -1); roam_feed(ROAM_EV_DISCONNECTED); }
                xSemaphoreGive(s_roam_mu);
            }
        } else if (s_want_sta) {
            ESP_LOGI(TAG, "disconnected, retry in %d ms", s_backoff_ms);
            esp_timer_stop(s_retry);
            esp_timer_start_once(s_retry, (uint64_t)s_backoff_ms * 1000);
            s_backoff_ms = s_backoff_ms * 2 > 60000 ? 60000 : s_backoff_ms * 2;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_backoff_ms = 1000;
        xEventGroupSetBits(s_eg, BIT_IP);
        ESP_LOGI(TAG, "got ip");
        if (s_roam_on && xSemaphoreTake(s_roam_mu, portMAX_DELAY) == pdTRUE) {
            if (s_roam_on) { esp_timer_stop(s_retry); roam_feed(ROAM_EV_GOT_IP); ROAM_HEAP("got_ip"); }
            xSemaphoreGive(s_roam_mu);
        }
        if (s_cb) s_cb(true);
    }
}

esp_err_t net_init(void) {
    if (s_inited) return ESP_OK;
    s_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    e = esp_wifi_init(&wc);
    if (e != ESP_OK) return e;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);   // 凭证只放我们自己的 petcfg
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);   // 含 v4.8 的 SCAN_DONE
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    const esp_timer_create_args_t ta = { .callback = retry_cb, .name = "wifi_retry" };
    esp_timer_create(&ta, &s_retry);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    setenv("TZ", "CST-8", 1); tzset();
    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    esp_netif_sntp_init(&sc);
    s_inited = true;
    return ESP_OK;
}

void net_sta_connect(const char *ssid, const char *pass) {
    net_roam_stop();   // 显式单组连接(配网页绑定/重登)接管,关掉漫游
    wifi_config_t w = { 0 };
    strlcpy((char *)w.sta.ssid, ssid, sizeof w.sta.ssid);
    strlcpy((char *)w.sta.password, pass ? pass : "", sizeof w.sta.password);
    w.sta.threshold.authmode = (pass && pass[0]) ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &w);
    s_want_sta = ssid && ssid[0];
    s_backoff_ms = 1000;
    if (s_want_sta) esp_wifi_connect();
}

bool net_sta_has_ip(void) { return s_eg && (xEventGroupGetBits(s_eg) & BIT_IP); }
bool net_wait_ip(int timeout_ms) {
    return xEventGroupWaitBits(s_eg, BIT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms)) & BIT_IP;
}

esp_err_t net_ap_start(char *name_out, size_t len) {
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    pet_ap_name(mac, name_out, len);   // v4.5.2:Passport-XXXX(原 PrintPal-XXXX)
    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, name_out, sizeof ap.ap.ssid);
    ap.ap.ssid_len = strlen(name_out);
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.max_connection = 2;
    ap.ap.channel = 6;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_err_t e = esp_wifi_set_config(WIFI_IF_AP, &ap);
    s_ap_on = e == ESP_OK;
    return e;
}

int net_scan(wifi_ap_record_t *out, int max) {
    if ((s_roam_on && s_roam.st == ROAM_SCANNING) || s_roam_scan_pending) return 0;   // v4.8:漫游的扫描正在进行或它的 SCAN_DONE 还没处理,配网页稍后重试(避免两路同时取/清扫描结果)
    s_scanning = true;
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_err_t e = esp_wifi_scan_start(&sc, true);
    s_scanning = false;
    if (e != ESP_OK) return 0;
    uint16_t n = 20;
    wifi_ap_record_t *tmp = calloc(n, sizeof(*tmp));
    if (!tmp) return 0;
    esp_wifi_scan_get_ap_records(&n, tmp);
    int k = 0;
    for (int i = 0; i < n && k < max; i++) {   // 已按 RSSI 排序;去掉空名和重名
        if (!tmp[i].ssid[0]) continue;
        bool dup = false;
        for (int j = 0; j < k; j++) if (!strcmp((char *)out[j].ssid, (char *)tmp[i].ssid)) dup = true;
        if (!dup) out[k++] = tmp[i];
    }
    free(tmp);
    return k;
}

// ---- v4.8 多组 Wi-Fi 入口 ----
bool net_roam_active(void) { return s_roam_on; }

void net_roam_stop(void) {
    if (!s_roam_mu) return;
    if (xSemaphoreTake(s_roam_mu, portMAX_DELAY) != pdTRUE) return;
    if (s_roam_on) { s_roam_on = false; esp_timer_stop(s_retry); s_scanning = false; }
    xSemaphoreGive(s_roam_mu);
}

// online=true:当前已经连着网,只换列表、不重扫(状态直接进 ONLINE,下次掉线才按 1 s 退避后重扫)。
static void roam_begin(wifi_slots_t *owned, bool online) {
    if (!s_roam_mu) s_roam_mu = xSemaphoreCreateMutex();
    if (!s_roam_mu || !owned || owned->n < 1) { free(owned); return; }
    xSemaphoreTake(s_roam_mu, portMAX_DELAY);
    free(s_slots);
    s_slots = owned;                         // 接管所有权;之后只读
    s_roam_on = true;
    s_want_sta = true;
    roam_init(&s_roam);
    ROAM_HEAP("roam_start");
    ESP_LOGI(TAG, "roam: start with %d saved networks%s", s_slots->n, online ? " (already online)" : "");
    if (online) s_roam.st = ROAM_ONLINE;
    else roam_feed(ROAM_EV_START);
    xSemaphoreGive(s_roam_mu);
}
void net_roam_start(wifi_slots_t *owned) { roam_begin(owned, false); }

bool net_sta_info(char *ssid_out, size_t len, int *rssi) {
    if (!net_sta_has_ip() || !ssid_out || !len) return false;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;
    strlcpy(ssid_out, (const char *)ap.ssid, len);
    if (rssi) *rssi = ap.rssi;
    return true;
}

bool net_try_connect(const char *ssid, const char *pass, int timeout_ms, int *reason) {
    net_roam_stop();
    esp_timer_stop(s_retry);
    s_want_sta = false;                      // 试连期间不要让 v4.7.x 的自动重连把 STA 拉回去
    s_try_reason = 0;
    s_try_on = true;
    wifi_config_t w = { 0 };
    strlcpy((char *)w.sta.ssid, ssid, sizeof w.sta.ssid);
    strlcpy((char *)w.sta.password, pass ? pass : "", sizeof w.sta.password);
    w.sta.threshold.authmode = (pass && pass[0]) ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    bool was = net_sta_has_ip();
    esp_wifi_disconnect();
    for (int i = 0; was && i < 50 && net_sta_has_ip(); i++) vTaskDelay(pdMS_TO_TICKS(20));   // 等旧连接的断开事件把 BIT_IP 清掉,否则会把旧网络的 IP 当成新网络连上了
    esp_wifi_set_config(WIFI_IF_STA, &w);
    vTaskDelay(pdMS_TO_TICKS(150));          // 让旧连接/旧尝试遗留的断开事件先被事件循环处理掉,再清标志,免得把它误当成这次的失败
    s_try_reason = 0;
    xEventGroupClearBits(s_eg, BIT_FAIL);
    ESP_LOGI(TAG, "try: connecting (timeout %d ms)", timeout_ms);   // 日志里不打 SSID/口令
    bool ok = false;
    if (esp_wifi_connect() == ESP_OK) {
        EventBits_t b = xEventGroupWaitBits(s_eg, BIT_IP | BIT_FAIL, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
        ok = b & BIT_IP;
    } else s_try_reason = 0;
    s_try_on = false;
    if (reason) *reason = ok ? 0 : s_try_reason;
    if (!ok) esp_wifi_disconnect();
    ESP_LOGI(TAG, "try: %s reason=%d", ok ? "ok" : "failed", ok ? 0 : s_try_reason);
    return ok;
}

void net_slots_apply(const wifi_slots_t *w, bool restart) {
    if (!w || w->n < 1) return;
    bool online = net_sta_has_ip();
    if (w->n == 1) {                         // 单组:v4.7.x 原来的路径
        net_roam_stop();
        if (restart || !online) net_sta_connect(w->s[0].ssid, w->s[0].pass);
        else { s_want_sta = true; s_backoff_ms = 1000; }   // 已经连着:不断线;STA 配置就是这一组(试连时写进去的)
        return;
    }
    wifi_slots_t *c = malloc(sizeof *c);
    if (!c) return;
    *c = *w;
    roam_begin(c, online && !restart);
}
