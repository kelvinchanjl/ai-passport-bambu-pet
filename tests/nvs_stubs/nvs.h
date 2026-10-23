// Host NVS stub backed by a key table extracted from a real NVS image (tests/test_nvs_compat.py).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
typedef struct { const char *ns, *key; char type; const char *s; int64_t i; } nvs_stub_item_t;   // type 's' or 'i'
extern const nvs_stub_item_t NVS_STUB_ITEMS[]; extern const size_t NVS_STUB_N;
esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h);
esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *out, size_t *len);
esp_err_t nvs_get_i64(nvs_handle_t h, const char *k, int64_t *v);
esp_err_t nvs_get_i32(nvs_handle_t h, const char *k, int32_t *v);
esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v);
esp_err_t nvs_set_i64(nvs_handle_t h, const char *k, int64_t v);
esp_err_t nvs_set_i32(nvs_handle_t h, const char *k, int32_t v);
esp_err_t nvs_erase_all(nvs_handle_t h);
esp_err_t nvs_commit(nvs_handle_t h);
void nvs_close(nvs_handle_t h);
