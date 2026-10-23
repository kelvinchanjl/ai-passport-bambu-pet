// main/devcap.h — 仅测试版:串口命令 + 屏幕抓图(流式,不占整帧缓冲)。正式版里是空函数。
#pragma once
#include <stdbool.h>
// LVGL 显示初始化之后、锁外调用:装 USB-Serial-JTAG 驱动、起读命令任务、挂 FLUSH_START 回调。
void devcap_init(void);
// pet_app 任务里每轮调用;有命令就返回 true 并把命令(去掉换行)拷到 out。
bool devcap_poll_cmd(char *out, int len);
// 在 LVGL 锁内调用:整屏重画一次,并把每次 flush 的像素(RGB565 小端,已做圆角遮罩)按行 base64 打到串口。
void devcap_capture(const char *tag);
