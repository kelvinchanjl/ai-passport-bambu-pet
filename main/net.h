// main/net.h — Wi-Fi:STA 自动重连(指数退避 1s→60s)+ 可选 SoftAP(配网)。
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "esp_wifi.h"
#include "wifi_slots.h"
esp_err_t net_init(void);
// 设置并连接 STA;ssid 为空时只开射频(用于扫描)。
void net_sta_connect(const char *ssid, const char *pass);
// v4.8 多组 Wi-Fi(>= 2 组才用):开机/断线后扫描 → 与已存 SSID 匹配 → 按信号强弱逐个试(每组 15 s)→ 全部失败按 1→60 s 退避后重扫。
// owned:堆上的列表,net.c 接管并负责 free。net_sta_connect() 会自动停止漫游(显式单组连接优先)。
void net_roam_start(wifi_slots_t *owned);
void net_roam_stop(void);
bool net_roam_active(void);
bool net_sta_has_ip(void);
// 等待拿到 IP,超时返回 false。
bool net_wait_ip(int timeout_ms);
// 开 SoftAP(开放热点),名字写入 name_out(如 Passport-3F2A)。
esp_err_t net_ap_start(char *name_out, size_t len);
// 扫描,返回条数(去重、按信号强度)。
int net_scan(wifi_ap_record_t *out, int max);
typedef void (*net_ip_cb_t)(bool up);
void net_set_ip_callback(net_ip_cb_t cb);
