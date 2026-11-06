// main/setup_portal.h — SoftAP + 强制门户配网(首次绑定 / 重新登录)。
#pragma once
#include "app_config.h"
#include <stdbool.h>
typedef enum { PORTAL_IDLE = 0, PORTAL_WAITING, PORTAL_BOUND } portal_state_t;
// reauth=true:只更新 token,Wi-Fi 和打印机不动;cfg 为当前配置(可写)。
bool setup_portal_start(app_config_t *cfg, bool reauth);
// 关 HTTP/DNS/SoftAP。不回滚 NVS:手机已经提交的 Wi-Fi 增删或绑定以提交为准。
void setup_portal_stop(void);
const char *setup_portal_ap_name(void);
portal_state_t setup_portal_state(void);
const char *setup_portal_bound_name(void);   // 绑定成功后显示的打印机名(ASCII)
