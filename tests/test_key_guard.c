// 按键唤醒守卫状态机的 host 测试:不依赖 SDK,直接验证 key_guard.h 的纯逻辑。
#include <assert.h>
#include <stdio.h>

#include "key_guard.h"

int main(void) {
    key_guard_t g = {0};

    // 未武装:不吞事件,也不改状态。
    assert(!key_guard_consume(&g, false, 0));
    assert(!g.armed);

    // 武装后:普通事件被吞,直到松手或到期。
    key_guard_arm(&g, 1000, 10000);
    assert(g.armed);
    assert(key_guard_consume(&g, false, 1500));   // 长按被吞
    assert(g.armed);
    assert(key_guard_consume(&g, false, 2500));   // 仍被吞
    assert(g.armed);

    // 松手事件撤防,且该事件本身也被吞。
    assert(key_guard_consume(&g, true, 2600));
    assert(!g.armed);
    assert(!key_guard_consume(&g, false, 2700));  // 之后正常派发

    // 到期兜底:没有松手事件也能撤防。
    key_guard_arm(&g, 1000, 10000);               // deadline = 11000
    assert(key_guard_consume(&g, false, 11001));  // 到期,吞掉这次
    assert(!g.armed);
    assert(!key_guard_consume(&g, false, 11002));

    // 释放早于初始化:按键驱动启动前就已松手,不会再有 release 事件。
    // 按采样到的物理状态(已松手)同步后立即撤防,下一次长按不再被吞。
    key_guard_arm(&g, 1000, 10000);
    key_guard_sync_released(&g, true);
    assert(!g.armed);
    assert(!key_guard_consume(&g, false, 1500));  // 下一次长按正常派发

    // 按住贯穿初始化:采样仍按住,守卫保持武装,直到松手事件。
    key_guard_arm(&g, 1000, 10000);
    key_guard_sync_released(&g, false);           // 仍按住,不撤防
    assert(g.armed);
    assert(key_guard_consume(&g, false, 1500));   // 长按仍被吞
    assert(g.armed);
    assert(key_guard_consume(&g, true, 2600));    // 松手撤防
    assert(!g.armed);

    puts("key wake guard state-machine tests: PASS");

    // ---- v4.8 休眠唤醒吞键守卫(key_wake_guard_*)----
    // 时间单位 us;键 0=上 1=下 2=中
    key_wake_guard_t w; bool wake;
    const int64_t MS = 1000;
    key_wake_guard_init(&w);
    // 亮屏、未武装:全部正常派发
    assert(!key_wake_guard_consume(&w, 0, KWG_EV_PRESS, false, 0, &wake) && !wake);
    assert(!key_wake_guard_consume(&w, 0, KWG_EV_RELEASE, false, 100 * MS, &wake));
    assert(!key_wake_guard_consume(&w, 0, KWG_EV_OTHER, false, 280 * MS, &wake));

    // 1) 上键唤醒:PRESS 吞并唤醒;RELEASE 吞;RELEASE 后 +180 ms 的 CLICK 吞(旧 key_guard 会漏这一下)
    int64_t t = 10000 * MS;
    assert(key_wake_guard_consume(&w, 0, KWG_EV_PRESS, true, t, &wake) && wake);
    assert(w.armed);
    assert(key_wake_guard_consume(&w, 0, KWG_EV_RELEASE, false, t + 90 * MS, &wake) && !wake);   // 此时已亮屏(asleep=false)
    assert(key_wake_guard_consume(&w, 0, KWG_EV_OTHER, false, t + 90 * MS + 180 * MS, &wake));   // CLICK @ release+180 ms
    assert(key_wake_guard_consume(&w, 0, KWG_EV_OTHER, false, t + 90 * MS + 249 * MS, &wake));   // 尾巴内:仍吞
    // 尾巴到期(release + 250 ms)后的下一次按键正常派发
    assert(!key_wake_guard_consume(&w, 1, KWG_EV_PRESS, false, t + 90 * MS + 250 * MS, &wake) && !wake);
    assert(!w.armed);
    assert(!key_wake_guard_consume(&w, 1, KWG_EV_RELEASE, false, t + 500 * MS, &wake));
    assert(!key_wake_guard_consume(&w, 1, KWG_EV_OTHER, false, t + 680 * MS, &wake));

    // 2) 中键长按唤醒:500 ms 的 LONG 吞(不会打开设置页),松开吞
    key_wake_guard_init(&w); t = 20000 * MS;
    assert(key_wake_guard_consume(&w, 2, KWG_EV_PRESS, true, t, &wake) && wake);
    assert(key_wake_guard_consume(&w, 2, KWG_EV_OTHER, false, t + 500 * MS, &wake) && !wake);    // LONG
    assert(key_wake_guard_consume(&w, 2, KWG_EV_OTHER, false, t + 5000 * MS, &wake));             // 按住 5 s 仍吞(长按没有短超时)
    assert(key_wake_guard_consume(&w, 2, KWG_EV_RELEASE, false, t + 6000 * MS, &wake));
    assert(!key_wake_guard_consume(&w, 2, KWG_EV_PRESS, false, t + 6000 * MS + 260 * MS, &wake));  // 尾巴后正常

    // 3) 中键双击唤醒:第二下按下/松开/DOUBLE 都吞(不会切换打印机),尾巴从第二次松开顺延
    key_wake_guard_init(&w); t = 30000 * MS;
    assert(key_wake_guard_consume(&w, 2, KWG_EV_PRESS, true, t, &wake) && wake);
    assert(key_wake_guard_consume(&w, 2, KWG_EV_RELEASE, false, t + 80 * MS, &wake));
    assert(key_wake_guard_consume(&w, 2, KWG_EV_PRESS, false, t + 200 * MS, &wake) && !wake);      // 第二下,在尾巴内
    assert(key_wake_guard_consume(&w, 2, KWG_EV_RELEASE, false, t + 280 * MS, &wake));
    assert(key_wake_guard_consume(&w, 2, KWG_EV_OTHER, false, t + 300 * MS, &wake));                // DOUBLE
    assert(key_wake_guard_consume(&w, 2, KWG_EV_OTHER, false, t + 280 * MS + 249 * MS, &wake));     // 顺延后的尾巴内
    assert(!key_wake_guard_consume(&w, 2, KWG_EV_PRESS, false, t + 280 * MS + 250 * MS, &wake));

    // 4) 别的键在尾巴内按下也吞;两个键都松开后才开始算尾巴
    key_wake_guard_init(&w); t = 40000 * MS;
    assert(key_wake_guard_consume(&w, 0, KWG_EV_PRESS, true, t, &wake) && wake);
    assert(key_wake_guard_consume(&w, 1, KWG_EV_PRESS, false, t + 50 * MS, &wake));
    assert(key_wake_guard_consume(&w, 0, KWG_EV_RELEASE, false, t + 100 * MS, &wake));
    assert(key_wake_guard_consume(&w, 1, KWG_EV_OTHER, false, t + 600 * MS, &wake));                // 下键还按着:不撤防
    assert(key_wake_guard_consume(&w, 1, KWG_EV_RELEASE, false, t + 700 * MS, &wake));
    assert(key_wake_guard_consume(&w, 1, KWG_EV_OTHER, false, t + 880 * MS, &wake));                // 下键 CLICK @ +180 ms
    assert(!key_wake_guard_consume(&w, 0, KWG_EV_PRESS, false, t + 950 * MS, &wake));               // 700+250 = 950 到期

    // 5) 卡键兜底 30 s
    key_wake_guard_init(&w); t = 50000 * MS;
    assert(key_wake_guard_consume(&w, 1, KWG_EV_PRESS, true, t, &wake) && wake);
    assert(key_wake_guard_consume(&w, 1, KWG_EV_OTHER, false, t + 29999 * MS, &wake));
    assert(!key_wake_guard_consume(&w, 1, KWG_EV_OTHER, false, t + 30000 * MS, &wake) && !w.armed);

    // 6) 熄屏、未武装时的非 PRESS(熄屏前按着的键的 RELEASE/CLICK):吞,但不唤醒
    key_wake_guard_init(&w);
    assert(key_wake_guard_consume(&w, 0, KWG_EV_RELEASE, true, 60000 * MS, &wake) && !wake && !w.armed);
    assert(key_wake_guard_consume(&w, 0, KWG_EV_OTHER, true, 60180 * MS, &wake) && !wake && !w.armed);
    // 武装中又睡着(极少见)再按:照样唤醒
    assert(key_wake_guard_consume(&w, 2, KWG_EV_PRESS, true, 70000 * MS, &wake) && wake);
    assert(key_wake_guard_consume(&w, 2, KWG_EV_PRESS, true, 70010 * MS, &wake) && wake);
    // 越界键号不越界访问
    key_wake_guard_init(&w);
    assert(key_wake_guard_consume(&w, 7, KWG_EV_PRESS, true, 0, &wake) && wake);
    assert(KWG_TAIL_US == 250000 && KWG_STUCK_US == 30000000);
    puts("first-press swallow (click +180 ms / long / double / 250 ms tail / stuck 30 s): PASS");
    return 0;
}
