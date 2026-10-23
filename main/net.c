// main/net.c
#include "net.h"
#include "pet_logic.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "net";
static EventGroupHandle_t s_eg;
#define BIT_IP BIT0
static bool s_inited, s_ap_on, s_want_sta;
static esp_timer_handle_t s_retry;
static int s_backoff_ms = 1000;
static net_ip_cb_t s_cb;
static bool s_scanning;

void net_set_ip_callback(net_ip_cb_t cb) { s_cb = cb; }

static void retry_cb(void *arg) { (void)arg; if (s_want_sta && !s_scanning) esp_wifi_connect(); }

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_want_sta) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        bool had = xEventGroupGetBits(s_eg) & BIT_IP;
        xEventGroupClearBits(s_eg, BIT_IP);
        if (had && s_cb) s_cb(false);
        if (s_want_sta) {
            ESP_LOGI(TAG, "disconnected, retry in %d ms", s_backoff_ms);
            esp_timer_stop(s_retry);
            esp_timer_start_once(s_retry, (uint64_t)s_backoff_ms * 1000);
            s_backoff_ms = s_backoff_ms * 2 > 60000 ? 60000 : s_backoff_ms * 2;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_backoff_ms = 1000;
        xEventGroupSetBits(s_eg, BIT_IP);
        ESP_LOGI(TAG, "got ip");
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
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
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
