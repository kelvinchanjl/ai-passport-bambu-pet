// main/setup_portal.c — 手机连工牌热点 → 弹出 portal.html → 发码/登录/选打印机 → 写 NVS → 重启。
// 接口(与 art/setup/portal.html 约定一致,/api/code 额外带 ssid/password,
// 因为首次配网时工牌要先连上家里 Wi-Fi 才能访问拓竹服务器):
//   GET  /api/scan                       -> [{ssid,rssi}]
//   POST /api/code  {account[,ssid,password]}
//   POST /api/login {account,code}       -> {devices:[{id,name}]}
//   POST /api/bind  {ssid,password,device_ids:[..≤4]} (旧版 device_id 仍接受) | {reauth:true}
//   POST /api/bind_lan {ssid,password,ip,code,sn}   (方案 B,局域网)
#include "lwip/inet.h"
#include "setup_portal.h"
#include "sdkconfig.h"
#include "bambu_api.h"
#include "net.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "portal";
extern const uint8_t portal_html_gz_start[] asm("_binary_portal_html_gz_start");
extern const uint8_t portal_html_gz_end[] asm("_binary_portal_html_gz_end");

static app_config_t *s_cfg;
static bool s_reauth;
static char s_ap[24], s_bound_name[48];
static volatile portal_state_t s_state;
static bambu_tokens_t *s_tok;          // 登录结果(堆,~3 KB)
static char s_uid[24], s_account[64];
static bambu_device_t s_devs[BAMBU_MAX_DEVICES];
static int s_ndev;
static char s_pend_ssid[33], s_pend_pass[65];

const char *setup_portal_ap_name(void) { return s_ap; }
portal_state_t setup_portal_state(void) { return s_state; }
const char *setup_portal_bound_name(void) { return s_bound_name; }

static cJSON *read_json(httpd_req_t *r) {
    if (r->content_len <= 0 || r->content_len > 2048) return NULL;
    char *b = malloc(r->content_len + 1);
    if (!b) return NULL;
    int got = 0;
    while (got < (int)r->content_len) {
        int n = httpd_req_recv(r, b + got, r->content_len - got);
        if (n <= 0) { free(b); return NULL; }
        got += n;
    }
    b[got] = 0;
    cJSON *j = cJSON_Parse(b); free(b);
    return j;
}
static const char *jstr(cJSON *j, const char *k) {
    const cJSON *v = cJSON_GetObjectItem(j, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}
static esp_err_t fail(httpd_req_t *r, const char *status, const char *msg) {
    httpd_resp_set_status(r, status);
    httpd_resp_set_type(r, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(r, msg);
}
static esp_err_t ok_json(httpd_req_t *r, const char *s) {
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, s);
}

static esp_err_t h_index(httpd_req_t *r) {
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, (const char *)portal_html_gz_start, portal_html_gz_end - portal_html_gz_start);
}
// 系统探测(generate_204 / hotspot-detect 等)和任何未知路径都跳到门户页
static esp_err_t h_redirect(httpd_req_t *r, httpd_err_code_t e) {
    (void)e;
    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Location", s_reauth ? "http://192.168.4.1/?mode=reauth" : "http://192.168.4.1/");
    return httpd_resp_send(r, NULL, 0);
}

static esp_err_t h_scan(httpd_req_t *r) {
    wifi_ap_record_t *aps = calloc(12, sizeof(*aps));
    if (!aps) return fail(r, "500 Internal Server Error", "内存不足");
    int n = net_scan(aps, 12);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        if (aps[i].primary > 14) continue;  // 只列 2.4G
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", (char *)aps[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", aps[i].rssi);
        cJSON_AddItemToArray(arr, o);
    }
    free(aps);
    char *s = cJSON_PrintUnformatted(arr); cJSON_Delete(arr);
    esp_err_t e = ok_json(r, s); free(s);
    return e;
}

static bool ensure_internet(const char *ssid, const char *pass) {
    if (net_sta_has_ip()) return true;
    if (ssid && ssid[0]) {
        strlcpy(s_pend_ssid, ssid, sizeof s_pend_ssid); strlcpy(s_pend_pass, pass ? pass : "", sizeof s_pend_pass);
        net_sta_connect(s_pend_ssid, s_pend_pass);
    }
    return net_wait_ip(15000);
}

static esp_err_t h_code(httpd_req_t *r) {
    cJSON *j = read_json(r);
    if (!j) return fail(r, "400 Bad Request", "请求格式不对");
    snprintf(s_account, sizeof s_account, "%s", jstr(j, "account"));
    bool up = s_reauth ? ensure_internet(NULL, NULL) : ensure_internet(jstr(j, "ssid"), jstr(j, "password"));
    cJSON_Delete(j);
    if (!up) return fail(r, "502 Bad Gateway", "工牌连不上 Wi-Fi,检查一下密码(只支持 2.4G)");
    char msg[96];
    if (bambu_api_send_code(s_account, msg, sizeof msg) != ESP_OK) return fail(r, "502 Bad Gateway", msg);
    return ok_json(r, "{}");
}

static esp_err_t h_login(httpd_req_t *r) {
    cJSON *j = read_json(r);
    if (!j) return fail(r, "400 Bad Request", "请求格式不对");
    // 只保留数字:iOS 短信自动填充/粘贴可能带空格或全角字符
    char code[12] = {0}; { const char *src = jstr(j, "code"); size_t k = 0;
        for (; *src && k < sizeof code - 1; src++) if (*src >= '0' && *src <= '9') code[k++] = *src; }
    if (jstr(j, "account")[0]) snprintf(s_account, sizeof s_account, "%s", jstr(j, "account"));
    cJSON_Delete(j);
    // 验证码是一次性的:门户页 oninput 可能对同一验证码重复提交(iOS 自动填充会触发两次 input),
    // 第二次会被拓竹判"验证码不对或已过期"并覆盖掉第一次的成功。同账号同验证码已成功过就直接复用结果。
    static char s_ok_code[12], s_ok_account[sizeof s_account];
    static char *s_ok_resp;
    if (s_ok_resp && s_tok && s_tok->token[0] && !strcmp(code, s_ok_code) && !strcmp(s_account, s_ok_account)) {
        ESP_LOGI(TAG, "login: duplicate submit of same code, reuse previous result");
        return ok_json(r, s_ok_resp);
    }
    ESP_LOGI(TAG, "login: code len=%d", (int)strlen(code));
    if (!net_sta_has_ip()) return fail(r, "502 Bad Gateway", "工牌还没连上 Wi-Fi");
    if (!s_tok) s_tok = calloc(1, sizeof(*s_tok));
    if (!s_tok) return fail(r, "500 Internal Server Error", "内存不足");
    char msg[96];
    if (bambu_api_login_code(s_account, code, s_tok, msg, sizeof msg) != ESP_OK) return fail(r, "401 Unauthorized", msg);
    if (bambu_api_get_uid(s_tok->token, s_uid, sizeof s_uid) != ESP_OK) return fail(r, "502 Bad Gateway", "读取账号信息失败");
    cJSON *o = cJSON_CreateObject(), *arr = cJSON_AddArrayToObject(o, "devices");
    if (!s_reauth) {
        if (bambu_api_list_devices(s_tok->token, s_devs, &s_ndev) != ESP_OK) { cJSON_Delete(o); return fail(r, "502 Bad Gateway", "读取打印机列表失败"); }
        for (int i = 0; i < s_ndev; i++) {
            cJSON *d = cJSON_CreateObject();
            char nm[80]; snprintf(nm, sizeof nm, "%s%s%s", s_devs[i].product, s_devs[i].product[0] ? " · " : "", s_devs[i].name);
            cJSON_AddStringToObject(d, "id", s_devs[i].id);
            cJSON_AddStringToObject(d, "name", nm);
            cJSON_AddBoolToObject(d, "online", s_devs[i].online);
            cJSON_AddItemToArray(arr, d);
        }
    }
    char *s = cJSON_PrintUnformatted(o); cJSON_Delete(o);
    esp_err_t e = ok_json(r, s);
    free(s_ok_resp); s_ok_resp = s;   // 留给重复提交复用
    strlcpy(s_ok_code, code, sizeof s_ok_code); strlcpy(s_ok_account, s_account, sizeof s_ok_account);
    return e;
}

static void reboot_task(void *arg) { (void)arg; vTaskDelay(pdMS_TO_TICKS(4000)); esp_restart(); }


static esp_err_t h_bind(httpd_req_t *r) {
    cJSON *j = read_json(r);
    if (!j) return fail(r, "400 Bad Request", "请求格式不对");
    if (!s_tok || !s_tok->token[0] || !s_uid[0]) { cJSON_Delete(j); return fail(r, "400 Bad Request", "请先登录"); }
    int64_t exp = s_tok->expires_in > 0 && time(NULL) > 1700000000 ? (int64_t)time(NULL) + s_tok->expires_in : 0;
#if CONFIG_PET_TEST_SHORT_EXPIRY
    for (int i = 0; i < 50 && time(NULL) < 1700000000; i++) vTaskDelay(pdMS_TO_TICKS(100));  // 等对时
    exp = time(NULL) > 1700000000 ? (int64_t)time(NULL) + 600 : 0;
    ESP_LOGW(TAG, "TEST build: token expiry overridden to now+600s");
#endif
    esp_err_t e;
    if (s_reauth || cJSON_IsTrue(cJSON_GetObjectItem(j, "reauth"))) {
        e = app_config_save_token(s_uid, s_tok->token, s_tok->refresh, exp);   // Wi-Fi/打印机不动
        snprintf(s_bound_name, sizeof s_bound_name, "%s", s_cfg->dev_name[0] ? s_cfg->dev_name : "");
    } else {
        // v4.2:勾选多台(最多 4 台,按页面顺序;第一台是开机默认显示的那台)
        app_config_t *c = s_cfg;
        int n = 0;
        const cJSON *ids = cJSON_GetObjectItem(j, "device_ids"), *it;
        cJSON *legacy = NULL;
        if (!cJSON_IsArray(ids) && jstr(j, "device_id")[0]) {
            legacy = cJSON_CreateArray(); cJSON_AddItemToArray(legacy, cJSON_CreateString(jstr(j, "device_id"))); ids = legacy;
        }
        cJSON_ArrayForEach(it, ids) {
            if (!cJSON_IsString(it) || n >= CFG_MAX_PRINTERS) continue;
            for (int i = 0; i < s_ndev; i++) {
                if (strcmp(s_devs[i].id, it->valuestring)) continue;
                bool dup = false;
                for (int k = 0; k < n; k++) if (!strcmp(c->printers[k].sn, s_devs[i].id)) dup = true;
                if (dup) break;
                strlcpy(c->printers[n].sn, s_devs[i].id, sizeof c->printers[n].sn);
                strlcpy(c->printers[n].name, s_devs[i].name, sizeof c->printers[n].name);   // 原名(含中文);屏幕缺字时 UI 用型号兜底
                strlcpy(c->printers[n].product, s_devs[i].product, sizeof c->printers[n].product);
                n++;
            }
        }
        cJSON_Delete(legacy);
        if (n == 0) { cJSON_Delete(j); return fail(r, "400 Bad Request", "请至少勾选一台打印机"); }
        c->n_printers = n;
        strlcpy(c->ssid, jstr(j, "ssid")[0] ? jstr(j, "ssid") : s_pend_ssid, sizeof c->ssid);
        strlcpy(c->pass, cJSON_GetObjectItem(j, "password") ? jstr(j, "password") : s_pend_pass, sizeof c->pass);
        strlcpy(c->sn, c->printers[0].sn, sizeof c->sn);
        c->lan_ip[0] = c->lan_code[0] = 0;   // 走云端就清掉方案 B
        strlcpy(c->dev_name, c->printers[0].name, sizeof c->dev_name);
        strlcpy(c->uid, s_uid, sizeof c->uid);
        strlcpy(c->token, s_tok->token, sizeof c->token);
        strlcpy(c->refresh, s_tok->refresh, sizeof c->refresh);
        c->token_exp = exp;
        e = app_config_save_all(c);
        if (n > 1) snprintf(s_bound_name, sizeof s_bound_name, "%d 台打印机", n);
        else snprintf(s_bound_name, sizeof s_bound_name, "%s", c->dev_name);
    }
    cJSON_Delete(j);
    if (e != ESP_OK) return fail(r, "500 Internal Server Error", "保存失败");
    memset(s_tok, 0, sizeof(*s_tok));
    s_state = PORTAL_BOUND;
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 2, NULL);
    return ok_json(r, "{}");
}

// 方案 B:局域网绑定,不需要拓竹账号。{ssid,password,ip,code,sn} 存 NVS 后重启进 LAN 模式
static bool all_alnum(const char *s, size_t lo, size_t hi) {
    size_t n = strlen(s); if (n < lo || n > hi) return false;
    for (; *s; s++) if (!((*s >= '0' && *s <= '9') || (*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z'))) return false;
    return true;
}
static esp_err_t h_bind_lan(httpd_req_t *r) {
    cJSON *j = read_json(r);
    if (!j) return fail(r, "400 Bad Request", "请求格式不对");
    const char *ssid = jstr(j, "ssid"), *ip = jstr(j, "ip"), *code = jstr(j, "code"), *sn = jstr(j, "sn");
    struct in_addr a;
    const char *bad = !ssid[0] ? "请选择 Wi-Fi" : inet_pton(AF_INET, ip, &a) != 1 ? "打印机 IP 格式不对,例如 192.168.1.23"
        : !all_alnum(code, 4, 15) ? "访问码不对(打印机屏幕上的 8 位码)" : !all_alnum(sn, 8, 31) ? "序列号不对(打印机设置里能看到)" : NULL;
    if (bad) { cJSON_Delete(j); return fail(r, "400 Bad Request", bad); }
    app_config_t *c = s_cfg;
    strlcpy(c->ssid, ssid, sizeof c->ssid);
    strlcpy(c->pass, jstr(j, "password"), sizeof c->pass);
    strlcpy(c->lan_ip, ip, sizeof c->lan_ip);
    strlcpy(c->lan_code, code, sizeof c->lan_code);
    strlcpy(c->sn, sn, sizeof c->sn);
    c->uid[0] = c->token[0] = c->refresh[0] = 0; c->token_exp = 0;   // 局域网不用云端 token,也不会有过期提醒
    strlcpy(c->dev_name, "LAN", sizeof c->dev_name);
    c->n_printers = 1;
    strlcpy(c->printers[0].sn, sn, sizeof c->printers[0].sn);
    strlcpy(c->printers[0].name, "局域网打印机", sizeof c->printers[0].name);
    c->printers[0].product[0] = 0;
    esp_err_t e = app_config_save_all(c);
    cJSON_Delete(j);
    if (e != ESP_OK) return fail(r, "500 Internal Server Error", "保存失败");
    snprintf(s_bound_name, sizeof s_bound_name, "%s", c->dev_name);
    s_state = PORTAL_BOUND;
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 2, NULL);
    return ok_json(r, "{}");
}

// 极简 DNS:所有 A 查询都回 192.168.4.1,触发手机的"登录此网络"弹窗
static void dns_task(void *arg) {
    (void)arg;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) < 0) { ESP_LOGE(TAG, "dns bind"); vTaskDelete(NULL); }
    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in from; socklen_t fl = sizeof from;
        int n = recvfrom(s, buf, sizeof buf - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 12) continue;
        buf[2] = 0x81; buf[3] = 0x80;                 // 标准应答,无错误
        buf[6] = 0; buf[7] = 1; buf[8] = buf[9] = buf[10] = buf[11] = 0;   // 1 个回答
        int p = 12;
        while (p < n && buf[p]) p += buf[p] + 1;
        p += 5;                                        // 0 + QTYPE + QCLASS
        if (p > n) continue;
        static const uint8_t ans[] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 192, 168, 4, 1 };
        memcpy(buf + p, ans, sizeof ans);
        sendto(s, buf, p + sizeof ans, 0, (struct sockaddr *)&from, fl);
    }
}

bool setup_portal_start(app_config_t *cfg, bool reauth) {
    s_cfg = cfg; s_reauth = reauth;
    if (net_init() != ESP_OK) return false;
    if (net_ap_start(s_ap, sizeof s_ap) != ESP_OK) return false;
    if (reauth && cfg->ssid[0] && !net_sta_has_ip()) net_sta_connect(cfg->ssid, cfg->pass);
    httpd_handle_t h = NULL;
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.stack_size = 8192;          // 处理函数里要跑 TLS 请求
    hc.max_open_sockets = 4;
    hc.lru_purge_enable = true;
    if (httpd_start(&h, &hc) != ESP_OK) return false;
    const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = h_index },
        { .uri = "/api/scan", .method = HTTP_GET, .handler = h_scan },
        { .uri = "/api/code", .method = HTTP_POST, .handler = h_code },
        { .uri = "/api/login", .method = HTTP_POST, .handler = h_login },
        { .uri = "/api/bind", .method = HTTP_POST, .handler = h_bind },
        { .uri = "/api/bind_lan", .method = HTTP_POST, .handler = h_bind_lan },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) httpd_register_uri_handler(h, &uris[i]);
    httpd_register_err_handler(h, HTTPD_404_NOT_FOUND, h_redirect);
    xTaskCreate(dns_task, "dns", 3072, NULL, 4, NULL);
    s_state = PORTAL_WAITING;
    ESP_LOGI(TAG, "portal up on %s (%s)", s_ap, reauth ? "reauth" : "setup");
    return true;
}
