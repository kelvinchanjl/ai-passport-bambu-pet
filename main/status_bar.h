// main/status_bar.h — v4.7 顶部状态条:左边时间(HH:MM,东八区),右边电池(图标 + 百分比)。
// 放在 lv_layer_top() 上,所有页面(宠物/详情/耗材/设置/配网/重新扫码/恢复出厂)都看得到。
#pragma once
#include <stdbool.h>

#define SB_Y        21          // 状态条中心线 = 机器名(HDR_Y=22)同一行
#define SB_NAME_L   54          // 机器名(含绿点)可用区间 [SB_NAME_L, SB_NAME_R]:左边时间到 x=51,右边 "100%" 从 x=163 起
#define SB_NAME_R   160

// 在 LVGL 锁内调用一次(pet_ui_create 之后)。
void status_bar_create(void);
// 在 pet_app 任务里(不持 LVGL 锁)每轮调用;内部限频 30 s 读一次电量(I2C)。
void status_bar_poll(void);

// 纯函数(host 单测用):格式化时间。t_valid=false 或 tm 为空时输出 "--:--"。
struct tm;
void status_bar_fmt_time(const struct tm *tm, bool t_valid, char *out, int len);
// 电量平滑:prev<0 表示还没有值;raw<0 表示本次读失败。返回新的平滑值(0..100)或 -1。
int status_bar_smooth(int prev, int raw);
