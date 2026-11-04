// main/key_guard.h —— 深睡按键唤醒后的"按键动作守卫"状态机(纯逻辑,便于 host 测试)。
//
// 深睡按键唤醒会让芯片在"唤醒键还按着"的状态下重启。按键组件晚几百毫秒才启动,
// 它看到的是一个已经按下的电平,并会在按住时长达到门限(本板 500ms)时判成长按,
// 于是几乎立刻触发一个意料外的动作。守卫在唤醒键松手前吞掉所有按键动作。
//
// 关键点(也是原实现的 bug):守卫的撤防不能只靠"收到 release 事件"。若用户在按键
// 组件开始观测前就松了手,组件从头到尾没看到按下,永远不会发 release,守卫就会一直
// 拦到 deadline 兜底。所以按键驱动初始化完成后,必须按采样到的物理电平同步一次:
// 已松手立即撤防(见 key_guard_sync_released)。
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    // armed 跨 main 任务与按键回调的 esp_timer 任务读写,标 volatile 以防被缓存。
    volatile bool armed;
    int64_t deadline_us;
} key_guard_t;

// 按键唤醒启动时武装。timeout_us 只作为按键卡住时的兜底。
static inline void key_guard_arm(key_guard_t *g, int64_t now_us, int64_t timeout_us) {
    g->armed = true;
    g->deadline_us = now_us + timeout_us;
}

// 每收到一个按键事件调一次。返回 true 表示该事件应被吞掉(不派发给应用),
// false 表示正常派发。release 事件或到期会就地撤防,但那次事件仍被吞掉。
static inline bool key_guard_consume(key_guard_t *g, bool is_release, int64_t now_us) {
    if (!g->armed) return false;
    if (is_release || now_us >= g->deadline_us) {
        g->armed = false;
    }
    return true;
}

// 按键驱动初始化完成后,按采样到的物理按键状态同步一次:
// released=true(已松手)立即撤防;released=false(仍按住)保持武装,直到收到
// release 事件或到期。采样失败时调用方应传 released=false,保守保持武装。
static inline void key_guard_sync_released(key_guard_t *g, bool released) {
    if (released) g->armed = false;
}

// ---------------------------------------------------------------------------
// v4.8 休眠唤醒吞键守卫(和上面的深睡守卫分开;上面那个在 RELEASE 时就撤防,不能直接用)。
//
// 按键组件(iot_button 4.2.0)事件顺序:PRESS → (按满 500 ms 出 LONG) → RELEASE → 约 180 ms 后才出
// CLICK;180 ms 内再按一次出 DOUBLE。上/下键响应 CLICK,中键由应用层 pet_okkey 判单击/双击(250 ms)。
// 所以如果在 RELEASE 时撤防,上/下键的 CLICK 会在唤醒的同时漏过去翻一页。
//
// 规则(在 on_key 里、事件进队列之前判断):
//   1. 熄屏时收到任何键的 PRESS:武装守卫,返回"吞掉 + 唤醒"。这次 PRESS 不进队列。
//   2. 守卫武装期间,所有键的 PRESS / LONG / RELEASE / CLICK / DOUBLE 全部吞掉。
//   3. 所有键都松开后留一个尾巴窗口:最后一次 RELEASE + KWG_TAIL_US(250 ms,取 180 ms 单击窗口和
//      250 ms 中键双击窗口里大的那个)。窗口内又按下:这次按下和它对应的松开也吞掉,尾巴顺延。
//   4. 尾巴到期并且没有键按着,守卫才撤防(在下一次事件到来时惰性判断)。
//   5. 卡键兜底 KWG_STUCK_US = 30 s:不能用短超时,否则按住超过超时后出来的 LONG 会漏过去打开设置页。
//   6. 熄屏但守卫没武装时收到的非 PRESS 事件(例如熄屏前就按着的键的 RELEASE/CLICK)也吞掉,但不唤醒。
// 亮屏、守卫撤防后:原样派发,不改变 500 ms 长按 / 250 ms 双击的行为。
// 状态只在按键回调(esp_timer 任务)里读写;asleep 由 main 任务写(volatile),先置位再关背光。
#define KWG_TAIL_US   250000
#define KWG_STUCK_US  30000000
#define KWG_KEYS      3

typedef enum { KWG_EV_PRESS = 0, KWG_EV_RELEASE, KWG_EV_OTHER } kwg_ev_t;   // OTHER = CLICK / DOUBLE / LONG

typedef struct {
    bool armed;
    bool down[KWG_KEYS];
    int64_t tail_until_us;   // 没有键按着时,尾巴结束时刻
    int64_t deadline_us;     // 卡键兜底
} key_wake_guard_t;

static inline void key_wake_guard_init(key_wake_guard_t *g) {
    g->armed = false;
    for (int i = 0; i < KWG_KEYS; i++) g->down[i] = false;
    g->tail_until_us = 0;
    g->deadline_us = 0;
}

static inline bool kwg_any_down(const key_wake_guard_t *g) {
    for (int i = 0; i < KWG_KEYS; i++) if (g->down[i]) return true;
    return false;
}

// 每个按键事件调一次。返回 true = 吞掉(不进队列)。*wake 置 true 表示这次按下应该唤醒屏幕。
static inline bool key_wake_guard_consume(key_wake_guard_t *g, int key, kwg_ev_t ev, bool asleep, int64_t now_us, bool *wake) {
    if (wake) *wake = false;
    if (key < 0 || key >= KWG_KEYS) key = 0;
    if (g->armed) {
        bool expired = now_us >= g->deadline_us || (!kwg_any_down(g) && now_us >= g->tail_until_us);
        if (expired) key_wake_guard_init(g);
    }
    if (!g->armed) {
        if (!asleep) return false;                 // 亮屏、没武装:正常派发
        if (ev != KWG_EV_PRESS) return true;       // 规则 6
        g->armed = true;                           // 规则 1
        g->down[key] = true;
        g->deadline_us = now_us + KWG_STUCK_US;
        g->tail_until_us = now_us + KWG_TAIL_US;   // 按着时不看尾巴;这里只是给个确定值
        if (wake) *wake = true;
        return true;
    }
    // 规则 2/3:武装中
    if (ev == KWG_EV_PRESS) {
        g->down[key] = true;
        if (asleep && wake) *wake = true;          // 又睡着了(极少见):照样唤醒
    } else if (ev == KWG_EV_RELEASE) {
        g->down[key] = false;
        if (!kwg_any_down(g)) g->tail_until_us = now_us + KWG_TAIL_US;
    }
    return true;
}
