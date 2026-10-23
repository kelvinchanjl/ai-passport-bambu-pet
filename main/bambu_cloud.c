// main/bambu_cloud.c — Wi-Fi STA + 拓竹云 MQTT(TLS 8883)→ status_store。
// 用户名 u_{uid},密码 = access token。v4.2:同一条连接订阅最多 4 台的 device/{SN}/report,连上后给每台发一次 pushall。
// 后台机器和前台一样只保留解析后的 printer_status_t(约 300 B/台),报文缓冲区共用一份。
// 拓竹推送是增量 JSON:只覆盖本条消息里出现的字段。
#include "bambu_parse.h"
#include "json_filter.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "status_source.h"
#include "sdkconfig.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "bambu_api.h"
#include "net.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "bambu";
#define MAX_MSG (64 * 1024)   // v4.4.3:原始报文上限(只是防呆;报文不再整条缓冲)
#define FILT_CAP (4 * 1024)   // v4.6 heap:H2C+4 AMS 约 2.2KB;6KB 余量过大,收到 4KB 省 2KB BSS

static app_config_t *s_cfg;
static esp_mqtt_client_handle_t s_mqtt;
static int s_n;
static char s_topic_report[PET_MAX_PRINTERS][48], s_topic_req[PET_MAX_PRINTERS][48], s_user[32];
static int s_rx_idx = -1;   // 当前分片报文属于哪台
// v4.4.3:MQTT 分片流式过滤,不缓冲整条报文(H2C 全量报文 12-22 KB,没有 PSRAM)
static jf_t s_jf; static char s_filt[FILT_CAP]; static bool s_jf_on;
static volatile bool s_parse_fail;   // 已连上但最近一条报文解析失败(且之后还没有成功的)
bool bambu_cloud_parse_failing(void) { return s_parse_fail; }
static printer_status_t s_st[PET_MAX_PRINTERS];
static bambu_parse_ctx_t s_pc[PET_MAX_PRINTERS];
static TaskHandle_t s_tok_task;
static volatile bool s_auth_failed;
static bool s_lan;

#define BAMBU_TOK_PRIO 5
#if CONFIG_PET_LOG_REPORT_KEYS
// v4.4.1 测试版:打印前 3 条报文的原文(去掉序列号/账号/token/摄像头地址等),每段 200 字节
static void scrub(cJSON *o) {
    static const char *K[] = { "sn", "dev_id", "user_id", "uid", "token", "access_code", "ipcam", "rtsp_url", "tutk_server", "net", "lan_ip", "ip", "mac", "wifi_signal", "fun", "cert_id" };
    for (cJSON *c = o ? o->child : NULL; c; ) {
        cJSON *n = c->next; bool drop = false;
        if (c->string) for (size_t i = 0; i < sizeof K / sizeof K[0]; i++) if (!strcmp(c->string, K[i])) drop = true;
        if (drop) cJSON_DeleteItemFromObjectCaseSensitive(o, c->string);
        else scrub(c);
        c = n;
    }
}
static int s_dumped;
static void dump_payload(int idx, cJSON *root, size_t len) {
    if (s_dumped >= 3) return;
    s_dumped++;
    scrub(root);
    char *t = cJSON_PrintUnformatted(root);
    if (!t) { ESP_LOGW(TAG, "dump: no mem"); return; }
    size_t n = strlen(t);
    ESP_LOGI(TAG, "payload #%d msg%d raw=%u B scrubbed=%u B", idx, s_dumped, (unsigned)len, (unsigned)n);
    for (size_t o = 0; o < n; o += 200) ESP_LOGI(TAG, "P%d.%03u %.200s", s_dumped, (unsigned)(o / 200), t + o);
    cJSON_free(t);
}
#endif
static int64_t s_last_pushall_us;
static bool s_got_full[PET_MAX_PRINTERS];
#if CONFIG_PET_LOG_REPORT_KEYS
static void heap_mark(const char *where) {
    ESP_LOGI(TAG, "heap@%s free=%u min=%u largest=%u", where,
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}
#endif
static void handle_json(int idx, const char *data, size_t len) {
#if CONFIG_PET_LOG_REPORT_KEYS
    if (len > 512) heap_mark("json_pre");
#endif
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) { s_parse_fail = true; ESP_LOGW(TAG, "bad json (%u B kept, heap free=%u largest=%u)", (unsigned)len,
                 (unsigned)esp_get_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)); return; }
    s_parse_fail = false;
    const cJSON *p = cJSON_GetObjectItem(root, "print");
    if (cJSON_IsObject(p)) {
        bambu_parse_print(&s_pc[idx], p, &s_st[idx]); status_store_set_idx(SRC_CLOUD, idx, &s_st[idx]);
        if (s_st[idx].n_nozzles > 0 && !s_got_full[idx]) {
            s_got_full[idx] = true; ESP_LOGI(TAG, "#%d first full report (temps present)", idx);
#if CONFIG_PET_LOG_REPORT_KEYS
            heap_mark("first_full");
#endif
        }
        pet_ams_t *am = status_store_ams_lock(idx);      // v4.3 耗材页
        if (am) { bambu_parse_ams(&s_pc[idx], p, am); status_store_ams_unlock(); }
#if CONFIG_PET_LOG_REPORT_KEYS
        // 字段核对:打印报文里出现的关键字段(不打印序列号/账号),方便对照 FIELDS_v4.2.md
        const cJSON *d = cJSON_GetObjectItem(p, "device");
        ESP_LOGI(TAG, "#%d keys: subtask_name=%d layer_num=%d mc_percent=%d ext=%d ctc=%d ams=%d vir_slot=%d", idx,
                 cJSON_HasObjectItem(p, "subtask_name"), cJSON_HasObjectItem(p, "layer_num"), cJSON_HasObjectItem(p, "mc_percent"),
                 d && cJSON_HasObjectItem(d, "extruder"), d && cJSON_HasObjectItem(d, "ctc"), cJSON_HasObjectItem(p, "ams"),
                 cJSON_HasObjectItem(p, "vir_slot"));
#endif
    }
#if CONFIG_PET_LOG_REPORT_KEYS
    dump_payload(idx, root, len);
#endif
    cJSON_Delete(root);
#if CONFIG_PET_LOG_REPORT_KEYS
    if (len > 512) heap_mark("json_post");
#endif
}
static void send_pushall(int i);
static void maybe_repush(int idx) {
    // 只收到增量报文(没温度)时,每 10 秒再要一次全量
    if (s_got_full[idx] || esp_timer_get_time() - s_last_pushall_us < 10000000) return;
    ESP_LOGI(TAG, "#%d no full report yet -> pushall again", idx);
    send_pushall(idx);
}

static void send_pushall(int i) {
    s_last_pushall_us = esp_timer_get_time();
    ESP_LOGI(TAG, "#%d pushall sent", i);
    esp_mqtt_client_publish(s_mqtt, s_topic_req[i],
        "{\"pushing\":{\"sequence_id\":\"0\",\"command\":\"pushall\",\"version\":1,\"push_target\":1}}", 0, 0, 0);
}
static int topic_idx(const char *t, int len) {
    for (int i = 0; i < s_n; i++) {
        size_t n = strlen(s_topic_report[i]);
        if ((int)n == len && !memcmp(t, s_topic_report[i], n)) return i;
    }
    return -1;
}
static void reset_all(void) {
    for (int i = 0; i < s_n; i++) {
        printer_status_reset(&s_st[i]); status_store_set_idx(SRC_CLOUD, i, &s_st[i]);
        pet_ams_t *am = status_store_ams_lock(i); if (am) { pet_ams_reset(am); status_store_ams_unlock(); }
    }
}

static void mqtt_ev(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base;
    esp_mqtt_event_handle_t e = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        status_store_set_link(SRC_CLOUD, LINK_ONLINE);
        for (int i = 0; i < s_n; i++) { s_got_full[i] = false; esp_mqtt_client_subscribe(s_mqtt, s_topic_report[i], 0); send_pushall(i); }
        break;
    case MQTT_EVENT_SUBSCRIBED:   // v4.4.1:订阅确认后再要一次全量(连上时那次可能早于订阅生效)
        ESP_LOGI(TAG, "MQTT subscribed (msg_id=%d)", e->msg_id);
        for (int i = 0; i < s_n; i++) if (!s_got_full[i]) send_pushall(i);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        s_jf_on = false; s_parse_fail = false;
        reset_all();
        if (!s_auth_failed) status_store_set_link(SRC_CLOUD, LINK_CONNECTING);
        break;
    case MQTT_EVENT_DATA:
        if (e->total_data_len > MAX_MSG) { if (e->current_data_offset == 0) ESP_LOGW(TAG, "msg too big (%d B), dropped", e->total_data_len); break; }
        if (e->current_data_offset == 0) {
            s_rx_idx = topic_idx(e->topic, e->topic_len);
            s_jf_on = s_rx_idx >= 0;
            if (!s_jf_on) break;
            jf_init(&s_jf, s_filt, sizeof s_filt, JF_BAMBU_KEYS, JF_BAMBU_N_KEYS);
        }
        if (!s_jf_on || s_rx_idx < 0) break;
        if ((size_t)e->current_data_offset != s_jf.in_total) {   // 丢了分片:这条作废
            ESP_LOGW(TAG, "#%d fragment gap (%d != %u), dropped", s_rx_idx, e->current_data_offset, (unsigned)s_jf.in_total);
            s_jf_on = false; break;
        }
        jf_feed(&s_jf, e->data, e->data_len);
        if (s_jf.in_total >= (size_t)e->total_data_len) {
            s_jf_on = false;
            if (!jf_finish(&s_jf)) {
                s_parse_fail = true;
                ESP_LOGW(TAG, "#%d bad json stream (raw %d B, kept %u B)", s_rx_idx, e->total_data_len, (unsigned)s_jf.len);
            } else {
#if CONFIG_PET_LOG_REPORT_KEYS
                ESP_LOGI(TAG, "#%d report raw=%d B kept=%u B heap free=%u min=%u", s_rx_idx, e->total_data_len, (unsigned)s_jf.len,
                         (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
#endif
                handle_json(s_rx_idx, s_filt, s_jf.len);
            }
            maybe_repush(s_rx_idx);
        }
        break;
    case MQTT_EVENT_ERROR:
        if (e->error_handle && e->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED &&
            (e->error_handle->connect_return_code == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED ||
             e->error_handle->connect_return_code == MQTT_CONNECTION_REFUSE_BAD_USERNAME)) {
            ESP_LOGW(TAG, "MQTT auth refused");
            s_auth_failed = true;
            xTaskNotifyGive(s_tok_task);   // 不在事件回调里停客户端;交给 token 任务
        } else ESP_LOGW(TAG, "MQTT error");
        break;
    default: break;
    }
}

static void start_mqtt(void) {
    ESP_LOGI(TAG, "MQTT connecting to %s:8883", s_lan ? "LAN" : s_cfg->host);
    if (s_mqtt) { esp_mqtt_client_reconnect(s_mqtt); return; }
    char cid[24]; snprintf(cid, sizeof cid, "pet_%08lx", (unsigned long)esp_random());
    esp_mqtt_client_config_t mc = {
        .broker.address = { .hostname = s_lan ? s_cfg->lan_ip : s_cfg->host, .port = 8883, .transport = MQTT_TRANSPORT_OVER_SSL },
        .credentials = { .username = s_user, .client_id = cid,
                         .authentication.password = s_lan ? s_cfg->lan_code : s_cfg->token },
        .session.keepalive = 60,
        .network = { .reconnect_timeout_ms = 10000, .timeout_ms = 15000 },
        .buffer = { .size = 4096, .out_size = 1024 },
        .task.stack_size = 6144,
    };
    if (s_lan) {
        // 打印机用自签名证书:局域网模式不校验证书(需 CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY),云端仍用证书包
        mc.broker.verification.skip_cert_common_name_check = true;
    } else {
        mc.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }
    s_mqtt = esp_mqtt_client_init(&mc);
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_ev, NULL);
    esp_mqtt_client_start(s_mqtt);
}

// ---- token 维护:鉴权失败或临近过期时静默刷新;刷新失败才提示重新扫码 ----
#define REFRESH_MARGIN_S (7 * 24 * 3600)

static bool try_refresh(void) {
    static bambu_tokens_t t;   // 3 KB,放静态区避免撑爆任务栈
    if (bambu_api_refresh(s_cfg->refresh, &t) != ESP_OK) return false;
    int64_t exp = t.expires_in > 0 ? (int64_t)time(NULL) + t.expires_in : 0;
    app_config_save_token(s_cfg->uid, t.token, t.refresh, exp);
    app_config_t *c = (app_config_t *)s_cfg;   // s_cfg 指向 main 的可写配置
    snprintf(c->token, sizeof c->token, "%s", t.token);
    snprintf(c->refresh, sizeof c->refresh, "%s", t.refresh);
    c->token_exp = exp;
    if (s_mqtt) {
        esp_mqtt_client_stop(s_mqtt);
        esp_mqtt_client_config_t upd = { .credentials.authentication.password = c->token };
        esp_mqtt_set_config(s_mqtt, &upd);
        esp_mqtt_client_start(s_mqtt);
    }
    ESP_LOGI(TAG, "token refreshed");
    return true;
}

// v4.2:开机后用 bind 列表刷新机器名(v4.1 存的是型号兜底名;用户在 Handy 里改名也能跟上)
static bool s_names_done;
static volatile bool s_need_start;
static void refresh_names(void) {
    if (s_names_done || s_lan || !s_cfg->token[0]) return;
    static bambu_device_t devs[BAMBU_MAX_DEVICES];
    int n = 0;
    if (bambu_api_list_devices(s_cfg->token, devs, &n) != ESP_OK) return;
    s_names_done = true;
    app_config_t *c = (app_config_t *)s_cfg;
    bool changed = false;
    if (c->n_printers == 0 && c->sn[0]) { c->n_printers = 1; snprintf(c->printers[0].sn, sizeof c->printers[0].sn, "%s", c->sn); }
    printer_info_t info[PET_MAX_PRINTERS];
    memset(info, 0, sizeof info);
    for (int i = 0; i < s_n; i++) {
        for (int k = 0; k < n; k++) {
            if (strcmp(devs[k].id, c->printers[i].sn)) continue;
            if (strcmp(c->printers[i].name, devs[k].name) || strcmp(c->printers[i].product, devs[k].product)) {
                snprintf(c->printers[i].name, sizeof c->printers[i].name, "%s", devs[k].name);
                snprintf(c->printers[i].product, sizeof c->printers[i].product, "%s", devs[k].product);
                changed = true;
            }
        }
        info[i] = (printer_info_t){ 0 };
        snprintf(info[i].name, sizeof info[i].name, "%s", c->printers[i].name);
        snprintf(info[i].product, sizeof info[i].product, "%s", c->printers[i].product);
    }
    status_store_set_info(SRC_CLOUD, s_n, info);
    if (changed) app_config_save_printers(c);
    ESP_LOGI(TAG, "bind list: %d devices, names %s", n, changed ? "updated" : "unchanged");
}

static void token_task(void *arg) {
    (void)arg;
    for (;;) {
        uint32_t why = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(6 * 3600 * 1000));
        if (!net_sta_has_ip()) continue;
        ESP_LOGI(TAG, "tok: woke (start=%d auth_failed=%d)", s_need_start, s_auth_failed);
        if (s_need_start) {
            refresh_names();
            s_need_start = false;
            if (!s_auth_failed) start_mqtt();
            why = 0;                         // 这次唤醒只是"连上网",不算鉴权失败
        }
        if (s_lan) continue;   // 局域网模式没有 token;访问码错了就一直打盹重试(见 README 方案 B)
#if CONFIG_PET_TEST_SHORT_EXPIRY
        bool need = s_auth_failed;   // 测试版:伪造的 10 分钟过期由 UI 处理,不去刷新真实 token
#else
        bool need = s_auth_failed || pet_token_needs_refresh(s_cfg->token_exp, time(NULL), REFRESH_MARGIN_S);
#endif
        if (!need) continue;
        bool ok = try_refresh();
        if (ok) s_auth_failed = false;
        else if (s_auth_failed || why) {
            ESP_LOGW(TAG, "token refresh failed -> need re-auth");
            if (s_mqtt) esp_mqtt_client_stop(s_mqtt);
            status_store_set_link(SRC_CLOUD, LINK_AUTH_FAIL);
        }
    }
}

static void on_ip(bool up) {
    // 先在 token 任务里拉一次 bind 列表(机器名),再起 MQTT:两条 TLS 不同时占内存
    if (up) { status_store_set_link(SRC_CLOUD, LINK_CONNECTING); s_need_start = true; xTaskNotifyGive(s_tok_task); }
    else {
        reset_all();                              // 断网时显示打盹,不报错
        status_store_set_link(SRC_CLOUD, LINK_CONNECTING);
    }
}

bool bambu_cloud_start(app_config_t *cfg) {
    s_lan = app_config_lan_ready(cfg);
    if (!s_lan && !app_config_cloud_ready(cfg)) { status_store_set_link(SRC_CLOUD, LINK_UNCONFIGURED); return false; }
    s_cfg = cfg;
    s_n = s_lan ? 1 : (cfg->n_printers > 0 ? cfg->n_printers : 1);   // 局域网模式只连一台
    if (s_n > PET_MAX_PRINTERS) s_n = PET_MAX_PRINTERS;
    printer_info_t info[PET_MAX_PRINTERS];
    memset(info, 0, sizeof info);
    for (int i = 0; i < s_n; i++) {
        const char *sn = cfg->n_printers > 0 ? cfg->printers[i].sn : cfg->sn;
        printer_status_reset(&s_st[i]);
        s_pc[i] = (bambu_parse_ctx_t)BAMBU_PARSE_CTX_INIT;
        snprintf(s_topic_report[i], sizeof s_topic_report[i], "device/%s/report", sn);
        snprintf(s_topic_req[i], sizeof s_topic_req[i], "device/%s/request", sn);
        if (cfg->n_printers > 0) {
            snprintf(info[i].name, sizeof info[i].name, "%s", cfg->printers[i].name);
            snprintf(info[i].product, sizeof info[i].product, "%s", cfg->printers[i].product);
        } else snprintf(info[i].name, sizeof info[i].name, "%s", cfg->dev_name);
    }
    status_store_set_info(SRC_CLOUD, s_n, info);
    if (s_lan) snprintf(s_user, sizeof s_user, "bblp");
    else snprintf(s_user, sizeof s_user, "u_%s", cfg->uid);
    ESP_LOGI(TAG, "mode: %s", s_lan ? "LAN (local MQTT)" : "cloud");
    status_store_set_link(SRC_CLOUD, LINK_CONNECTING);
    if (net_init() != ESP_OK) return false;
    xTaskCreate(token_task, "bambu_tok", 6144, NULL, BAMBU_TOK_PRIO, &s_tok_task);   // v4.4.1:高于 LVGL(4),界面忙也不会饿死云端
    net_set_ip_callback(on_ip);
    net_sta_connect(cfg->ssid, cfg->pass);
    return true;
}
