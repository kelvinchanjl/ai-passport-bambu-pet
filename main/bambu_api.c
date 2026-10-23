// main/bambu_api.c
#include "bambu_api.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "bambu_api";
#define RESP_MAX (16 * 1024)
static bool s_unauth;
bool bambu_api_last_unauthorized(void) { return s_unauth; }

// 返回 HTTP 状态码,body 由调用方 free。失败返回 -1。
static int request(const char *method, const char *path, const char *token, const char *body, char **resp) {
    char url[160];
    snprintf(url, sizeof url, BAMBU_API_HOST "%s", path);
    *resp = NULL;
    esp_http_client_config_t cfg = {
        .url = url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 15000,
        .buffer_size = 2048, .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    esp_http_client_set_method(c, strcmp(method, "POST") == 0 ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "User-Agent", "bambu_network_agent/01.09.05.01");
    if (token && token[0]) {
        size_t n = strlen(token) + 8;
        char *auth = malloc(n);
        if (auth) { snprintf(auth, n, "Bearer %s", token); esp_http_client_set_header(c, "Authorization", auth); free(auth); }
    }
    int blen = body ? (int)strlen(body) : 0;
    int status = -1;
    if (esp_http_client_open(c, blen) == ESP_OK) {
        if (blen) esp_http_client_write(c, body, blen);
        esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        char *buf = malloc(RESP_MAX + 1); int total = 0;
        if (buf) {
            int r;
            while (total < RESP_MAX && (r = esp_http_client_read(c, buf + total, RESP_MAX - total)) > 0) total += r;
            buf[total] = 0; *resp = buf;
        }
    }
    esp_http_client_cleanup(c);
    s_unauth = status == 401 || status == 403;
    ESP_LOGI(TAG, "%s %s -> %d", method, path, status);  // 不打印 body(含 token)
    return status;
}

static void copy_msg(cJSON *root, char *errmsg, size_t len, const char *dflt) {
    if (!errmsg) return;
    const cJSON *m = cJSON_GetObjectItem(root, "message");
    if (!cJSON_IsString(m)) m = cJSON_GetObjectItem(root, "error");
    snprintf(errmsg, len, "%s", cJSON_IsString(m) && m->valuestring[0] ? m->valuestring : dflt);
}

esp_err_t bambu_api_send_code(const char *account, char *errmsg, size_t errlen) {
    cJSON *b = cJSON_CreateObject();
    bool email = strchr(account, '@') != NULL;
    cJSON_AddStringToObject(b, email ? "email" : "phone", account);
    cJSON_AddStringToObject(b, "type", "codeLogin");
    char *s = cJSON_PrintUnformatted(b); cJSON_Delete(b);
    char *resp;
    int st = request("POST", email ? "/v1/user-service/user/sendemail/code" : "/v1/user-service/user/sendsmscode", NULL, s, &resp);
    free(s);
    esp_err_t r = (st >= 200 && st < 300) ? ESP_OK : ESP_FAIL;
    if (r != ESP_OK && errmsg) {
        cJSON *j = resp ? cJSON_Parse(resp) : NULL;
        copy_msg(j, errmsg, errlen, st < 0 ? "连不上拓竹服务器" : "发送验证码失败");
        cJSON_Delete(j);
    }
    free(resp);
    return r;
}

static esp_err_t parse_tokens(const char *resp, bambu_tokens_t *out) {
    cJSON *j = resp ? cJSON_Parse(resp) : NULL;
    if (!j) return ESP_FAIL;
    const cJSON *a = cJSON_GetObjectItem(j, "accessToken"), *rf = cJSON_GetObjectItem(j, "refreshToken");
    const cJSON *ex = cJSON_GetObjectItem(j, "expiresIn");
    esp_err_t r = ESP_FAIL;
    if (cJSON_IsString(a) && a->valuestring[0] && strlen(a->valuestring) < sizeof out->token) {
        snprintf(out->token, sizeof out->token, "%s", a->valuestring);
        snprintf(out->refresh, sizeof out->refresh, "%s", cJSON_IsString(rf) ? rf->valuestring : "");
        out->expires_in = cJSON_IsNumber(ex) ? (int64_t)ex->valuedouble : 0;
        r = ESP_OK;
    }
    cJSON_Delete(j);
    return r;
}

esp_err_t bambu_api_login_code(const char *account, const char *code, bambu_tokens_t *out, char *errmsg, size_t errlen) {
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "account", account);
    cJSON_AddStringToObject(b, "code", code);
    char *s = cJSON_PrintUnformatted(b); cJSON_Delete(b);
    char *resp;
    int st = request("POST", "/v1/user-service/user/login", NULL, s, &resp);
    free(s);
    esp_err_t r = (st >= 200 && st < 300) ? parse_tokens(resp, out) : ESP_FAIL;
    if (r != ESP_OK && errmsg) {
        cJSON *j = resp ? cJSON_Parse(resp) : NULL;
        copy_msg(j, errmsg, errlen, st < 0 ? "连不上拓竹服务器" : "验证码不对或已过期");
        cJSON_Delete(j);
        ESP_LOGW(TAG, "login failed: http=%d msg=%s", st, errmsg);
    }
    free(resp);
    return r;
}

esp_err_t bambu_api_refresh(const char *refresh, bambu_tokens_t *out) {
    if (!refresh || !refresh[0]) return ESP_ERR_INVALID_ARG;
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "refreshToken", refresh);
    char *s = cJSON_PrintUnformatted(b); cJSON_Delete(b);
    char *resp;
    int st = request("POST", "/v1/user-service/user/refreshtoken", NULL, s, &resp);
    free(s);
    esp_err_t r = (st >= 200 && st < 300) ? parse_tokens(resp, out) : ESP_FAIL;
    if (r == ESP_OK && !out->refresh[0]) snprintf(out->refresh, sizeof out->refresh, "%s", refresh);
    free(resp);
    return r;
}

esp_err_t bambu_api_get_uid(const char *token, char *uid, size_t len) {
    char *resp;
    int st = request("GET", "/v1/design-user-service/my/preference", token, NULL, &resp);
    esp_err_t r = ESP_FAIL;
    cJSON *j = (st == 200 && resp) ? cJSON_Parse(resp) : NULL;
    const cJSON *u = j ? cJSON_GetObjectItem(j, "uid") : NULL;
    if (cJSON_IsNumber(u)) { snprintf(uid, len, "%.0f", u->valuedouble); r = ESP_OK; }
    else if (cJSON_IsString(u)) { snprintf(uid, len, "%s", u->valuestring); r = ESP_OK; }
    cJSON_Delete(j); free(resp);
    return r;
}

esp_err_t bambu_api_list_devices(const char *token, bambu_device_t *out, int *count) {
    *count = 0;
    char *resp;
    int st = request("GET", "/v1/iot-service/api/user/bind", token, NULL, &resp);
    cJSON *j = (st == 200 && resp) ? cJSON_Parse(resp) : NULL;
    free(resp);
    if (!j) return ESP_FAIL;
    const cJSON *d;
    cJSON_ArrayForEach(d, cJSON_GetObjectItem(j, "devices")) {
        if (*count >= BAMBU_MAX_DEVICES) break;
        const cJSON *id = cJSON_GetObjectItem(d, "dev_id"), *nm = cJSON_GetObjectItem(d, "name");
        const cJSON *pr = cJSON_GetObjectItem(d, "dev_product_name"), *on = cJSON_GetObjectItem(d, "online");
        if (!cJSON_IsString(id)) continue;
        bambu_device_t *o = &out[(*count)++];
        memset(o, 0, sizeof(*o));
        snprintf(o->id, sizeof o->id, "%s", id->valuestring);
        snprintf(o->name, sizeof o->name, "%s", cJSON_IsString(nm) ? nm->valuestring : id->valuestring);
        snprintf(o->product, sizeof o->product, "%s", cJSON_IsString(pr) ? pr->valuestring : "");
        o->online = cJSON_IsTrue(on);
    }
    cJSON_Delete(j);
    return ESP_OK;
}
