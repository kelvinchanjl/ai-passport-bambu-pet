// main/app_config.c
#include "app_config.h"
#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <string.h>

#define NS "petcfg"

static void get(nvs_handle_t h, bool open, const char *key, char *dst, size_t len, const char *fallback) {
    size_t n = len;
    esp_err_t e = open ? nvs_get_str(h, key, dst, &n) : ESP_FAIL;
    // v4.5.1b:读不到时记下原因(NOT_FOUND = 键不存在/已被擦除;INVALID_LENGTH = 值比缓冲区长),便于区分"数据没了"和"读法变了"
    if (open && e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) ESP_LOGW("cfg", "nvs get %s: %s", key, esp_err_to_name(e));
    if (e != ESP_OK || dst[0] == 0) snprintf(dst, len, "%s", fallback);
}

void app_config_load(app_config_t *c) {
    memset(c, 0, sizeof(*c));
    nvs_handle_t h = 0;
    esp_err_t oe = nvs_open(NS, NVS_READONLY, &h);
    bool open = oe == ESP_OK;
    if (!open) ESP_LOGW("cfg", "nvs_open(%s) = %s", NS, esp_err_to_name(oe));
    get(h, open, "ssid", c->ssid, sizeof c->ssid, CONFIG_PET_WIFI_SSID);
    get(h, open, "pass", c->pass, sizeof c->pass, CONFIG_PET_WIFI_PASS);
    get(h, open, "uid", c->uid, sizeof c->uid, CONFIG_PET_BAMBU_UID);
    get(h, open, "token", c->token, sizeof c->token, CONFIG_PET_BAMBU_TOKEN);
    get(h, open, "refresh", c->refresh, sizeof c->refresh, "");
    get(h, open, "sn", c->sn, sizeof c->sn, CONFIG_PET_BAMBU_SN);
    get(h, open, "host", c->host, sizeof c->host, CONFIG_PET_BAMBU_HOST);
    get(h, open, "dname", c->dev_name, sizeof c->dev_name, "");
    get(h, open, "lan_ip", c->lan_ip, sizeof c->lan_ip, "");
    get(h, open, "lan_code", c->lan_code, sizeof c->lan_code, "");
    // v4.2 多机列表
    static char sns[CFG_MAX_PRINTERS * 33], names[CFG_MAX_PRINTERS * 49], prods[CFG_MAX_PRINTERS * 25];
    get(h, open, "sns", sns, sizeof sns, "");
    get(h, open, "dnames", names, sizeof names, "");
    get(h, open, "dprods", prods, sizeof prods, "");
    c->n_printers = 0;
    char *sp = sns, *np = names, *pp = prods;
    while (sp && *sp && c->n_printers < CFG_MAX_PRINTERS) {
        cfg_printer_t *p = &c->printers[c->n_printers];
        char *se = strchr(sp, ','); if (se) *se = 0;
        char *ne = np ? strchr(np, '\n') : NULL; if (ne) *ne = 0;
        char *pe = pp ? strchr(pp, '\n') : NULL; if (pe) *pe = 0;
        strlcpy(p->sn, sp, sizeof p->sn);
        strlcpy(p->name, np ? np : "", sizeof p->name);
        strlcpy(p->product, pp ? pp : "", sizeof p->product);
        if (p->sn[0]) c->n_printers++;
        sp = se ? se + 1 : NULL; np = ne ? ne + 1 : NULL; pp = pe ? pe + 1 : NULL;
    }
    if (c->n_printers == 0 && c->sn[0]) {   // v4.1 及以前:单台
        c->n_printers = 1;
        snprintf(c->printers[0].sn, sizeof c->printers[0].sn, "%s", c->sn);
        snprintf(c->printers[0].name, sizeof c->printers[0].name, "%s", c->dev_name);
    }
    if (c->n_printers > 0) snprintf(c->sn, sizeof c->sn, "%s", c->printers[0].sn);
    if (open) { nvs_get_i64(h, "texp", &c->token_exp); nvs_get_i32(h, "remday", &c->remind_day); nvs_close(h); }
    ESP_LOGI("cfg", "ssid=%s uid=%s token=%s refresh=%s sn=%s host=%s", c->ssid[0] ? "set" : "-",
             c->uid[0] ? "set" : "-", c->token[0] ? "set" : "-", c->refresh[0] ? "set" : "-",
             c->sn[0] ? "set" : "-", c->host);
    ESP_LOGI("cfg", "printers=%d", c->n_printers);
}

bool app_config_lan_ready(const app_config_t *c) { return c->ssid[0] && c->lan_ip[0] && c->lan_code[0] && c->sn[0]; }

esp_err_t app_config_save_remind_day(int32_t day) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_i32(h, "remday", day);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

bool app_config_wifi_ready(const app_config_t *c) { return c->ssid[0] != 0; }
bool app_config_cloud_ready(const app_config_t *c) {
    return c->ssid[0] && c->uid[0] && c->token[0] && c->sn[0] && c->host[0];
}

static esp_err_t put_token(nvs_handle_t h, const char *uid, const char *token, const char *refresh, int64_t exp) {
    esp_err_t e = nvs_set_str(h, "uid", uid);
    if (e == ESP_OK) e = nvs_set_str(h, "token", token);
    if (e == ESP_OK) e = nvs_set_str(h, "refresh", refresh ? refresh : "");
    if (e == ESP_OK) e = nvs_set_i64(h, "texp", exp);
    return e;
}

esp_err_t app_config_save_all(const app_config_t *c) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_str(h, "ssid", c->ssid);
    if (e == ESP_OK) e = nvs_set_str(h, "pass", c->pass);
    if (e == ESP_OK) e = nvs_set_str(h, "sn", c->sn);
    if (e == ESP_OK) e = nvs_set_str(h, "dname", c->dev_name);
    if (e == ESP_OK) e = nvs_set_str(h, "lan_ip", c->lan_ip);     // 云端绑定时为空 = 清掉旧的 LAN 配置
    if (e == ESP_OK) e = nvs_set_str(h, "lan_code", c->lan_code);
    if (e == ESP_OK) e = put_token(h, c->uid, c->token, c->refresh, c->token_exp);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e == ESP_OK) e = app_config_save_printers(c);
    return e;
}

esp_err_t app_config_save_printers(const app_config_t *c) {
    static char sns[CFG_MAX_PRINTERS * 33], names[CFG_MAX_PRINTERS * 49], prods[CFG_MAX_PRINTERS * 25];
    sns[0] = names[0] = prods[0] = 0;
    for (int i = 0; i < c->n_printers && i < CFG_MAX_PRINTERS; i++) {
        const char *sep = i ? "," : "", *nl = i ? "\n" : "";
        strlcat(sns, sep, sizeof sns); strlcat(sns, c->printers[i].sn, sizeof sns);
        strlcat(names, nl, sizeof names); strlcat(names, c->printers[i].name, sizeof names);
        strlcat(prods, nl, sizeof prods); strlcat(prods, c->printers[i].product, sizeof prods);
    }
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_str(h, "sns", sns);
    if (e == ESP_OK) e = nvs_set_str(h, "dnames", names);
    if (e == ESP_OK) e = nvs_set_str(h, "dprods", prods);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

esp_err_t app_config_save_token(const char *uid, const char *token, const char *refresh, int64_t exp) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = put_token(h, uid, token, refresh, exp);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

esp_err_t app_config_erase(void) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_erase_all(h);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}
