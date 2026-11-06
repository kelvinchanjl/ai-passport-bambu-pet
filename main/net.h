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
// ---- 配网页 Wi-Fi 管理(v4.8)----
// 当前连着的网络:拿到 IP 才算连着;ssid_out 至少 33 字节,rssi 是当前这条链路的信号(esp_wifi_sta_get_ap_info)。没连着返回 false。
bool net_sta_info(char *ssid_out, size_t len, int *rssi);
// 试连一组凭证:停掉漫游,断开当前连接,连新的,最多等 timeout_ms 拿到 IP。成功返回 true,STA 留在新网络上;
// 失败返回 false,*reason 是最后一次 STA 断开原因码(0 = 超时没有任何断开事件)。调用方负责之后调 net_slots_apply() 恢复。会阻塞调用线程。
bool net_try_connect(const char *ssid, const char *pass, int timeout_ms, int *reason);
// 列表变了(增/删/试连失败后恢复)之后让连接逻辑跟上。restart=true:现在就重新选网;false:保持当前连接,只是把列表换成新的。
// 1 组 → 单组路径;>= 2 组 → 漫游(false 且当前在线时直接进入 ONLINE,不重扫)。w 由调用方保留所有权(内部复制)。
void net_slots_apply(const wifi_slots_t *w, bool restart);
bool net_sta_has_ip(void);
// 等待拿到 IP,超时返回 false。
bool net_wait_ip(int timeout_ms);
// 开 SoftAP(开放热点),名字写入 name_out(如 Passport-3F2A)。
esp_err_t net_ap_start(char *name_out, size_t len);
// 关 SoftAP,回到纯 STA(已连上的家里 Wi-Fi 不断)。未开过热点则空操作。
void net_ap_stop(void);
bool net_ap_active(void);
// 扫描,返回条数(去重、按信号强度)。
int net_scan(wifi_ap_record_t *out, int max);
typedef void (*net_ip_cb_t)(bool up);
void net_set_ip_callback(net_ip_cb_t cb);
