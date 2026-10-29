// main/wifi_slots.h — v4.8 多组 Wi-Fi:纯逻辑(列表增删/迁移/序列化/NVS 占用计算),不含任何 ESP-IDF 依赖,主机可测。
//
// 存储:NVS 命名空间 "petcfg" 的 blob 键 "wifis" = 版本 1 B + 组数 1 B + 组数 × 98 B(ssid[33] + pass[65],定长、尾部补 0)。
//   旧键 "ssid"/"pass"(v4.7.x 的单组)继续保留,始终镜像第 0 组:
//     - 降级回 v4.7.x 仍能连第 0 组;
//     - 旧固件改了 ssid/pass 之后再升级,开机对账(wifi_slots_reconcile)会把它提到第 0 组;
//     - 配网页绑定(h_bind/h_bind_lan)照旧只写 ssid/pass,下次开机由对账并入列表,不需要改绑定代码。
//   NVS 占用(32 B/项):blob = 1 个头 + ceil(字节数/32) 个数据项 + 1 个 index 项。
//     1 组 = 100 B → 6 项;3 组 = 296 B → 12 项;5 组(上限)= 492 B → 18 项。完整数字见 wifi_nvs_blob_entries()。
//   另有整数键 "wmig"(1 项):升级迁移后置 1,用户在配网页成功增/删/点完成后清 0,配网页据此显示一次性「之前保存的 Wi-Fi 已自动加入」横幅。
//   所以 5 组满额 = blob 18 项 + wmig 1 项 = 19 项(旧键 ssid/pass 是 v4.7.x 就有的,不算新增)。
//   写入新 blob 时旧 blob 在新 blob 提交前仍占着项,瞬时峰值约 2 倍(5 组 = 36 项),远小于 24 KB NVS 分区的 630 个可用项。
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WIFI_SLOTS_MAX   5     // 上限 5 组(PM 定)
#define WIFI_SSID_MAX    32    // 802.11 SSID 最长 32 字节
#define WIFI_PASS_MAX    64    // WPA 口令最长 63 个字符,或 64 位十六进制 PSK
#define WIFI_BLOB_VER    1
#define WIFI_SLOT_BYTES  (WIFI_SSID_MAX + 1 + WIFI_PASS_MAX + 1)          // 98
#define WIFI_BLOB_MAX    (2 + WIFI_SLOTS_MAX * WIFI_SLOT_BYTES)           // 492

typedef struct { char ssid[WIFI_SSID_MAX + 1]; char pass[WIFI_PASS_MAX + 1]; } wifi_slot_t;
typedef struct { int n; wifi_slot_t s[WIFI_SLOTS_MAX]; } wifi_slots_t;

typedef enum { WIFI_OK = 0, WIFI_E_INVALID, WIFI_E_FULL, WIFI_E_NOTFOUND, WIFI_E_LAST } wifi_res_t;

void wifi_slots_init(wifi_slots_t *w);
// SSID 1..32 字节;口令 0..64 字节(0 = 开放网络)。NULL 口令按空处理。
bool wifi_slots_valid(const char *ssid, const char *pass);
// 配网页提交口令的格式校验(比 wifi_slots_valid 严):空串(开放网络)、8–63 个字符的 WPA 口令、或恰好 64 位十六进制 PSK。1–7 位一定连不上,提前拒绝。
bool wifi_pass_ok(const char *pass);
int  wifi_slots_find(const wifi_slots_t *w, const char *ssid);   // 找不到 -1
// 新增;同名 SSID = 更新口令(位置不变)。满 5 组且是新 SSID → WIFI_E_FULL。
wifi_res_t wifi_slots_add(wifi_slots_t *w, const char *ssid, const char *pass);
// 按 SSID 删除。只剩最后一组时拒绝(WIFI_E_LAST):没有 Wi-Fi 就没有云端,工牌会掉回配网。
wifi_res_t wifi_slots_delete(wifi_slots_t *w, const char *ssid);
// 绑定/对账用:放到第 0 组(已存在则移到最前并更新口令);满了就挤掉最后一组。返回是否有变化。
bool wifi_slots_promote(wifi_slots_t *w, const char *ssid, const char *pass);
// 开机对账:旧键 ssid/pass(legacy)非空且不等于第 0 组 → promote。返回是否需要写回 NVS。
bool wifi_slots_reconcile(wifi_slots_t *w, const char *legacy_ssid, const char *legacy_pass);

// blob 序列化。pack 返回字节数(cap 不够返回 0);unpack 严格校验(版本/组数/长度/字符串结尾/SSID 非空),失败返回 false 且 w 为空。
size_t wifi_slots_pack(const wifi_slots_t *w, uint8_t *buf, size_t cap);
bool   wifi_slots_unpack(wifi_slots_t *w, const uint8_t *buf, size_t len);

// NVS 占用(32 B 一项,规则同 ESP-IDF nvs_page.cpp 的 writeItem):字符串 = 1 + ceil((len+1)/32);blob = 1 + ceil(bytes/32) + 1。
size_t wifi_nvs_blob_bytes(int n);        // 2 + 98n
int    wifi_nvs_blob_entries(int n);      // 6 / 9 / 12 / 15 / 18  (n = 1..5)
int    wifi_nvs_str_entries(size_t len);
