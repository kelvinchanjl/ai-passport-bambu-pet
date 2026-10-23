// main/bambu_api.h — 拓竹国内云 HTTPS 接口(api.bambulab.cn,非官方公开接口)。
// 阻塞调用,只能在工作任务里用(不要在按键回调/LVGL 任务里调)。
#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define BAMBU_API_HOST "https://api.bambulab.cn"
#define BAMBU_MAX_DEVICES 12   // 配网页列出账号里全部机器(勾选上限 4 台见 CFG_MAX_PRINTERS)

typedef struct { char id[32]; char name[48]; char product[24]; bool online; } bambu_device_t;
typedef struct {
    char token[1600], refresh[1600];
    int64_t expires_in;    // 秒
} bambu_tokens_t;

// 发验证码:account 含 '@' 走邮箱接口,否则当手机号走短信接口。
esp_err_t bambu_api_send_code(const char *account, char *errmsg, size_t errlen);
// 验证码登录,成功填 tokens。
esp_err_t bambu_api_login_code(const char *account, const char *code, bambu_tokens_t *out, char *errmsg, size_t errlen);
// 用 refresh token 换新 token(社区记录显示此接口可能一直返回 401,失败时调用方走重新扫码)。
esp_err_t bambu_api_refresh(const char *refresh, bambu_tokens_t *out);
// uid(纯数字字符串)
esp_err_t bambu_api_get_uid(const char *token, char *uid, size_t len);
// 已绑定打印机列表
esp_err_t bambu_api_list_devices(const char *token, bambu_device_t *out, int *count);
// 401/403 = token 无效
bool bambu_api_last_unauthorized(void);
