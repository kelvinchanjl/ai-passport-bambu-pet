// main/sleep_logic.h — v4.8 休眠(熄屏)的纯逻辑:何时允许/进入休眠、三种熄屏画面各要做什么动作、
// 60 s 刷新节拍与防残影位移、B 画面显示哪台、打印状态变化时自动亮屏。不依赖 ESP-IDF/LVGL,有主机测试。
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "pet_logic.h"
#include "pet_ui.h"
#include "sleep_cfg.h"

#define SLEEP_BL_AWAKE_PCT  80      // 亮屏背光(和 main.c 开机值一致)
#define SLEEP_BL_DIM_PCT    8       // B/C 低背光(PM 定 5–10%;同一套 LEDC 10 位、5 kHz,duty = 1023*8/100 = 81)
#define SLEEP_REFRESH_MS    60000   // B/C 每 60 s 刷新一次
#define SLEEP_SHIFT_EVERY   5       // 每 5 分钟(5 次刷新)防残影移位一次
#define SLEEP_SHIFT_PX      3

// 不休眠的场景:配网/热点/二维码页、绑定页、重新登录、恢复出厂确认、演示模式、设置为「关」。
bool pet_sleep_allowed(ui_screen_t sc, bool portal_on, bool demo, uint8_t sleep_min);

// 无操作计时。只有按键(和自动亮屏事件)清零;不允许休眠时计时暂停,离开后从头计。
typedef struct { bool asleep; uint32_t idle_since_ms; } pet_sleep_t;
typedef enum { PET_SLEEP_EV_NONE = 0, PET_SLEEP_EV_ENTER } pet_sleep_ev_t;
void pet_sleep_init(pet_sleep_t *s, uint32_t now_ms);
void pet_sleep_activity(pet_sleep_t *s, uint32_t now_ms);
pet_sleep_ev_t pet_sleep_tick(pet_sleep_t *s, uint32_t now_ms, bool allowed, uint8_t sleep_min);
void pet_sleep_set_awake(pet_sleep_t *s, uint32_t now_ms);   // 唤醒:清 asleep 并重新计时

// 进入休眠时页面怎么变:设置页开着先关掉(回到打开设置之前的页面),页面本身(首页/详情/耗材)不变;唤醒后就是这一页。
typedef struct { int page; bool settings; } pet_view_t;
pet_view_t pet_sleep_view_on_enter(pet_view_t v);

// 每种熄屏画面要做的动作(进入时;唤醒时反过来)。只有 A 发 SLPIN/SLPOUT。
typedef struct {
    uint8_t backlight_pct;   // 熄屏期间背光
    bool panel_sleep;        // A:DISPOFF + SLPIN;唤醒 SLPOUT + 整屏刷新后 DISPON
    bool pause_render;       // A:暂停 LVGL 刷屏(关掉失效登记 + 暂停刷新定时器)
    bool sleep_screen;       // B/C:建独立熄屏页面,60 s 刷新
    bool hide_status_bar;    // B/C:隐藏 lv_layer_top 上的状态条
    bool stop_pet_anim;      // 角色动画/闪烁定时器停掉(A/B/C 都停,省 CPU)
} pet_sleep_actions_t;
pet_sleep_actions_t pet_sleep_actions(uint8_t face);

// 60 s 刷新节拍:已对时 = 分钟变了就刷(每分钟一次,时间不会慢);没对时 = 距上次 >= 60 s。
// minute_key = 时*60+分,没对时传 -1。
bool pet_sleep_refresh_due(int last_key, int now_key, uint32_t last_ms, uint32_t now_ms);
// 防残影:已对时 = 分钟数整除 5;没对时 = 每 5 次刷新。refresh_n 为本次是第几次刷新(从 1 起)。
bool pet_sleep_shift_due(int minute_key, int refresh_n);
// 位置 pos(0..3)对应 (−3,−3) → (+3,−3) → (+3,+3) → (−3,+3);pos<0 = 居中 (0,0)
void pet_sleep_shift_xy(int pos, int *dx, int *dy);

// B 画面显示哪台:当前选中的那台在打印就显示它,否则列表里第一台在打印的;都不在打印返回 -1。
int pet_sleep_pick_printer(const pet_state_t *states, int n, int cur);
// B 版式(中心线 y):打印中 时间 118 / 电量 154;不打印 时间 136 / 电量 172(UI 定稿 v2)
void pet_sleep_b_layout(bool printing, int *time_cy, int *batt_cy);

// 自动亮屏:只在「变成」完成/失败/暂停/断料时触发;第一次看到的状态(prev == PET_STATE_COUNT,开机/重连/机器数变化后)不算。
bool pet_wake_on_state(pet_state_t prev, pet_state_t now);
// 扫一遍所有机器,更新 prev[],返回触发次数;*pause_idx = 最后一台「变成暂停」的机器(没有 = -1)。
int pet_wake_check(pet_state_t *prev, const pet_state_t *now, int n, int *pause_idx);
void pet_wake_reset(pet_state_t *prev);   // 全部置为「没见过」

// 设置页文案:「休眠：关 / 2 分钟 / 5 分钟」「熄屏画面：全黑 / 时钟 / 线线」(全角冒号,数字两侧半角空格)
const char *pet_set_sleep_label(uint8_t sleep_min);
const char *pet_set_face_label(uint8_t sleep_face);

// B/C 标签上实际会写的文案(纯函数,主机可测)。UI 层用同一套,避免主机测不到的字串路径。
void pet_sleep_fmt_time(bool synced, int hour, int min, char *out, size_t len);   // "HH:MM" 或 "--:--"
void pet_sleep_fmt_batt(int soc, char *out, size_t len);                           // "N%" 或 "--%"
void pet_sleep_fmt_pct(int pct, char *out, size_t len);                            // 打印进度 "N%"

typedef struct {
    bool uses_sleep_screen;   // B/C 建独立页面
    bool pause_render;        // 只有 A
    bool show_time;
    bool show_batt;
    bool show_pet;            // C:idle 第 0 帧
    bool show_print_row;      // B 且正在打印:机器名 + 进度
    char time[8];
    char batt[8];
    char name[64];
    char pct[8];
    int time_cy, batt_cy;
} pet_sleep_view_t;

// 按三种熄屏画面「会建/更新哪些标签」组成一帧要写的文本。name_ok = 原名每个字都在字库里。
void pet_sleep_compose_view(uint8_t face, const char *time, int soc, bool printing,
                            const char *name, const char *product, int idx, int pct,
                            bool name_ok, pet_sleep_view_t *out);
