// main/app_config.h — 配置:NVS 命名空间 "petcfg" 优先,空则用 menuconfig 编译期值。
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#define CFG_MAX_PRINTERS 4
typedef struct { char sn[32]; char name[48]; char product[24]; } cfg_printer_t;
typedef struct {
    char ssid[33], pass[65];
    char uid[24], token[1600], refresh[1600], sn[32], host[64], dev_name[48];
    char lan_ip[40], lan_code[16];   // 方案 B:局域网模式(有值则走本地 MQTT)
    int64_t token_exp;
    int n_printers;                         // v4.2 多机:配网页勾选的打印机(最多 4 台);printers[0].sn == sn
    cfg_printer_t printers[CFG_MAX_PRINTERS];
    int32_t remind_day;  // 上次过期提醒的本地日序号   // token 过期时间(Unix 秒);0 = 未知
} app_config_t;
void app_config_load(app_config_t *c);
bool app_config_wifi_ready(const app_config_t *c);
bool app_config_cloud_ready(const app_config_t *c);   // Wi-Fi + uid + token + SN
bool app_config_lan_ready(const app_config_t *c);     // Wi-Fi + lan_ip + lan_code + SN
esp_err_t app_config_save_remind_day(int32_t day);
// 首次绑定:写全部字段
esp_err_t app_config_save_all(const app_config_t *c);
// 重新登录/刷新:只更新 token 相关字段,Wi-Fi 和打印机不动
esp_err_t app_config_save_token(const char *uid, const char *token, const char *refresh, int64_t exp);
// 恢复出厂:清空 petcfg 命名空间(不做整片擦除)
esp_err_t app_config_erase(void);
// v4.2:NVS "sns"(逗号分隔序列号) / "dnames"(\n 分隔机器名) / "dprods"(\n 分隔型号)。旧配置只有 sn/dname 时自动当成 1 台。
esp_err_t app_config_save_printers(const app_config_t *c);
