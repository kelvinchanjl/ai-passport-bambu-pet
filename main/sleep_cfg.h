// main/sleep_cfg.h — v4.8 休眠设置的取值与校验(纯逻辑,主机可测;app_config.c 和 sleep_logic.c 共用)。
// NVS:petcfg/sleep_min(u8:0=关 / 2 / 5 分钟,默认 2)、petcfg/sleep_face(u8:0=A 全黑 / 1=B 时钟 / 2=C 线线,默认 0)。
// 读不到或读到别的值:sleep_min 按 2,sleep_face 按 0。恢复出厂会擦掉 petcfg,两项随之回到默认。
#pragma once
#include <stdint.h>

#define SLEEP_MIN_OFF      0
#define SLEEP_MIN_DEFAULT  2
#define SLEEP_FACE_BLACK   0   // A 全黑:背光 0 + 面板 SLPIN + 暂停 LVGL 刷屏
#define SLEEP_FACE_CLOCK   1   // B 暗屏时钟:不 SLPIN,低背光,每 60 s 刷新
#define SLEEP_FACE_PET     2   // C 暗屏睡觉线线:不 SLPIN,低背光,每 60 s 刷新
#define SLEEP_FACE_COUNT   3
#define SLEEP_FACE_DEFAULT SLEEP_FACE_BLACK

static inline uint8_t sleep_min_sanitize(uint8_t v) { return (v == 0 || v == 2 || v == 5) ? v : SLEEP_MIN_DEFAULT; }
static inline uint8_t sleep_face_sanitize(uint8_t v) { return v < SLEEP_FACE_COUNT ? v : SLEEP_FACE_DEFAULT; }
// 设置页中键循环:关 → 2 分钟 → 5 分钟 → 关;全黑 → 时钟 → 线线 → 全黑。非法输入先按默认再走一步。
static inline uint8_t sleep_min_next(uint8_t v) { v = sleep_min_sanitize(v); return v == 0 ? 2 : v == 2 ? 5 : 0; }
static inline uint8_t sleep_face_next(uint8_t v) { return (uint8_t)((sleep_face_sanitize(v) + 1) % SLEEP_FACE_COUNT); }
