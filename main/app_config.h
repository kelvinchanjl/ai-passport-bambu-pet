// main/app_config.h — 配置:NVS 命名空间 "petcfg" 优先,空则用 menuconfig 编译期值。
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "wifi_slots.h"
#include "sleep_cfg.h"
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
    uint8_t sleep_min;   // v4.8 休眠:0=关 / 2 / 5 分钟(NVS petcfg/sleep_min,默认 2)
    uint8_t sleep_face;  // v4.8 熄屏画面:0=全黑 1=时钟 2=线线(NVS petcfg/sleep_face,默认 0)
} app_config_t;
void app_config_load(app_config_t *c);
bool app_config_wifi_ready(const app_config_t *c);
bool app_config_cloud_ready(const app_config_t *c);   // Wi-Fi + uid + token + SN
bool app_config_lan_ready(const app_config_t *c);     // Wi-Fi + lan_ip + lan_code + SN
esp_err_t app_config_save_remind_day(int32_t day);
// v4.8 休眠设置:写入前先校验(非法值按默认写),各占 1 个 NVS 条目
esp_err_t app_config_save_sleep_min(uint8_t v);
esp_err_t app_config_save_sleep_face(uint8_t v);
// 首次绑定:写全部字段
esp_err_t app_config_save_all(const app_config_t *c);
// 重新登录/刷新:只更新 token 相关字段,Wi-Fi 和打印机不动
esp_err_t app_config_save_token(const char *uid, const char *token, const char *refresh, int64_t exp);
// 恢复出厂:清空 petcfg 命名空间(不做整片擦除)
esp_err_t app_config_erase(void);
// v4.2:NVS "sns"(逗号分隔序列号) / "dnames"(\n 分隔机器名) / "dprods"(\n 分隔型号)。旧配置只有 sn/dname 时自动当成 1 台。
esp_err_t app_config_save_printers(const app_config_t *c);
// v4.8 多组 Wi-Fi(最多 WIFI_SLOTS_MAX = 5 组,存 NVS petcfg/"wifis";格式和 NVS 占用见 wifi_slots.h)。
// load:读列表并与旧键 ssid/pass(c->ssid/c->pass)对账——第一次启动把 v4.7.x 的单组迁成第 0 组;有变化才写回。
//       NVS 打不开时返回错误,但 s 里仍有旧键那一组(可用来连网)。
// save:先镜像第 0 组到旧键 ssid/pass(降级回 v4.7.x 仍能连),再写 blob。列表为空时旧键写成空串。
esp_err_t app_config_wifi_load(wifi_slots_t *s, const app_config_t *c);
esp_err_t app_config_wifi_save(const wifi_slots_t *s);
// 升级迁移标记(NVS 整数键 "wmig",1 项):load 在「blob 不存在、从旧键迁移」时置 1;配网页 /api/wifi_list 的 migrated 就是它。
// clear:用户在配网页成功增/删/点「完成」后清 0。bound:配网页首次绑定/局域网绑定后调用——立刻把新绑定的 Wi-Fi 写成第 0 组,并清标记(新用户不该看到"已自动加入"横幅)。
bool app_config_wifi_migrated(void);
esp_err_t app_config_wifi_clear_migrated(void);
esp_err_t app_config_wifi_bound(const app_config_t *c);
