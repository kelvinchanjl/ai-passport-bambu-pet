// tests/test_nvs_compat.c — app_config_load() against keys extracted from a real (v4.4.1-format) NVS image.
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "nvs.h"
#include "app_config.h"
static const char *s_ns;
esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h) {
    (void)m; for (size_t i = 0; i < NVS_STUB_N; i++) if (!strcmp(NVS_STUB_ITEMS[i].ns, ns)) { s_ns = ns; *h = 1; return ESP_OK; }
    return ESP_ERR_NVS_NOT_FOUND;
}
static const nvs_stub_item_t *find(const char *k, char t) {
    for (size_t i = 0; i < NVS_STUB_N; i++) if (!strcmp(NVS_STUB_ITEMS[i].ns, s_ns) && !strcmp(NVS_STUB_ITEMS[i].key, k) && NVS_STUB_ITEMS[i].type == t) return &NVS_STUB_ITEMS[i];
    return NULL;
}
esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *out, size_t *len) {
    (void)h; const nvs_stub_item_t *it = find(k, 's'); if (!it) return ESP_ERR_NVS_NOT_FOUND;
    size_t n = strlen(it->s) + 1; if (n > *len) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, it->s, n); *len = n; return ESP_OK;
}
esp_err_t nvs_get_i64(nvs_handle_t h, const char *k, int64_t *v) { (void)h; const nvs_stub_item_t *it = find(k, 'i'); if (!it) return ESP_ERR_NVS_NOT_FOUND; *v = it->i; return ESP_OK; }
esp_err_t nvs_get_i32(nvs_handle_t h, const char *k, int32_t *v) { (void)h; const nvs_stub_item_t *it = find(k, 'i'); if (!it) return ESP_ERR_NVS_NOT_FOUND; *v = (int32_t)it->i; return ESP_OK; }
esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v) { (void)h; (void)k; (void)v; return ESP_OK; }
esp_err_t nvs_set_i64(nvs_handle_t h, const char *k, int64_t v) { (void)h; (void)k; (void)v; return ESP_OK; }
esp_err_t nvs_set_i32(nvs_handle_t h, const char *k, int32_t v) { (void)h; (void)k; (void)v; return ESP_OK; }
esp_err_t nvs_erase_all(nvs_handle_t h) { (void)h; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
int main(int argc, char **argv) {
    static app_config_t c; app_config_load(&c);
    int expect_ok = argc > 1 && !strcmp(argv[1], "ok");
    if (expect_ok) {
        assert(c.ssid[0] && c.uid[0] && c.token[0] && c.refresh[0] && c.sn[0] && c.n_printers == 1);
        assert(app_config_cloud_ready(&c));
        assert(!strcmp(c.printers[0].product, "H2C"));
        assert(strlen(c.token) == 1100);
    } else assert(!app_config_cloud_ready(&c));
    printf("test_nvs_compat(%s): PASS\n", expect_ok ? "v4.4.1 image" : "erased petcfg");
    return 0;
}
