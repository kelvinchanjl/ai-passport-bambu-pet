// main/net.h — Wi-Fi:STA 自动重连(指数退避 1s→60s)+ 可选 SoftAP(配网)。
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "esp_wifi.h"
esp_err_t net_init(void);
// 设置并连接 STA;ssid 为空时只开射频(用于扫描)。
void net_sta_connect(const char *ssid, const char *pass);
bool net_sta_has_ip(void);
// 等待拿到 IP,超时返回 false。
bool net_wait_ip(int timeout_ms);
// 开 SoftAP(开放热点),名字写入 name_out(如 Passport-3F2A)。
esp_err_t net_ap_start(char *name_out, size_t len);
// 扫描,返回条数(去重、按信号强度)。
int net_scan(wifi_ap_record_t *out, int max);
typedef void (*net_ip_cb_t)(bool up);
void net_set_ip_callback(net_ip_cb_t cb);
