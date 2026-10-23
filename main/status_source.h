// main/status_source.h — 打印机状态来源抽象:演示数据 / 拓竹云 MQTT。v4.2:最多 4 台打印机。
#pragma once
#include "pet_logic.h"
#include "app_config.h"

typedef enum { SRC_DEMO = 0, SRC_CLOUD } status_src_t;
typedef struct {
    char name[48];          // 云端 bind 列表里的机器名(如「我的 H2C」)
    char product[24];       // 型号(字库缺字时的兜底显示)
} printer_info_t;

void status_store_init(void);
// 当前选中机器(兼容 v4.1 调用)
void status_store_get(printer_status_t *out, link_state_t *link);
// 第 idx 台;online = 链路在线且 5 分钟内收到过它的报文
void status_store_get_idx(int idx, printer_status_t *out, link_state_t *link, bool *online, printer_info_t *info);
int  status_store_count(void);
int  status_store_current(void);
void status_store_select(int idx);
// 只有当前激活来源的写入才生效
void status_store_set(status_src_t src, const printer_status_t *s);            // = set_idx(src, 0, s)
void status_store_set_idx(status_src_t src, int idx, const printer_status_t *s);
void status_store_set_info(status_src_t src, int n, const printer_info_t *info);
void status_store_set_link(status_src_t src, link_state_t link);
void status_store_activate(status_src_t src);
status_src_t status_store_active(void);
// v4.3 耗材页:云端第 idx 台的 AMS(拿着存储锁,解析完调 unlock);get_ams 取当前来源的一份拷贝(演示 = pet_fake_ams)
pet_ams_t *status_store_ams_lock(int idx);
void status_store_ams_unlock(void);
void status_store_get_ams(int idx, pet_ams_t *out);
uint32_t status_store_version(void);

// 演示源:第 1 台的状态只在单击中键时切换(v4.4 起不再 3 秒自动轮换);v4.4 起演示只有这一台(去掉了示例 P1S)。demo_next() 立即切到下一种。
void demo_source_start(void);
void demo_source_stop(void);
void demo_next(void);
bool demo_source_running(void);
int demo_step(void);   // 0..5 = 空闲/打印中/完成/暂停/断料/失败

// 拓竹云源。配置不完整时返回 false 且不连网。
bool bambu_cloud_start(app_config_t *cfg);
bool bambu_cloud_parse_failing(void);   // v4.4.3:已连上但最近一条报文解析失败
