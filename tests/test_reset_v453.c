// tests/test_reset_v453.c — v4.5.3 恢复出厂防误触:状态机 + 设置页不回绕
#include <assert.h>
#include <stdio.h>
#include "pet_logic.h"
#include "pet_ui.h"
int main(void) {
    pet_reset_t r;
    // 1) 长按进设置页带过来的按住:打开确认页时中键还按着 -> 不计时,按多久都不清除
    pet_reset_init(&r); pet_reset_open(&r, true);
    for (uint32_t t = 0; t <= 20000; t += 100) assert(pet_reset_tick(&r, t) == PET_RST_EV_NONE && pet_reset_elapsed(&r, t) == 0);
    pet_reset_press(&r, 1000);                                   // 没见到松手之前的按下也不算
    assert(pet_reset_tick(&r, 9000) == PET_RST_EV_NONE);
    puts("carried-over hold does not arm: PASS");
    // 2) 必须先松开再按下;计时从 0 开始
    assert(pet_reset_release(&r, 20000) == PET_RST_EV_NONE && r.st == PET_RST_READY);
    assert(pet_reset_tick(&r, 30000) == PET_RST_EV_NONE);         // 只是松开:不计时
    pet_reset_press(&r, 30000); assert(pet_reset_elapsed(&r, 30000) == 0 && pet_reset_elapsed(&r, 31000) == 1000);
    puts("release then press required, counts from 0: PASS");
    // 3) 提前松手 = 取消
    assert(pet_reset_release(&r, 33000) == PET_RST_EV_CANCEL && r.st == PET_RST_OFF);
    assert(pet_reset_tick(&r, 40000) == PET_RST_EV_NONE);
    pet_reset_press(&r, 41000); assert(pet_reset_tick(&r, 47000) == PET_RST_EV_NONE);   // 取消后再按:不会接着算
    puts("early release cancels: PASS");
    // 4) 4.9 秒不清除,5 秒清除(只报一次)
    pet_reset_init(&r); pet_reset_open(&r, false);                // 单击打开(中键已松开)
    pet_reset_press(&r, 100000);
    assert(pet_reset_tick(&r, 104900) == PET_RST_EV_NONE);
    assert(pet_reset_tick(&r, 104999) == PET_RST_EV_NONE);
    assert(pet_reset_tick(&r, 105000) == PET_RST_EV_RESET);
    assert(pet_reset_tick(&r, 106000) == PET_RST_EV_NONE);
    pet_reset_init(&r); pet_reset_open(&r, false); pet_reset_press(&r, 0);
    assert(pet_reset_tick(&r, 4900) == PET_RST_EV_NONE && pet_reset_release(&r, 4900) == PET_RST_EV_CANCEL);
    pet_reset_open(&r, false); pet_reset_press(&r, 0xFFFFF000u); assert(pet_reset_tick(&r, 0xFFFFF000u + 5000) == PET_RST_EV_RESET);   // 计时器回绕
    puts("4.9 s no reset, 5 s reset: PASS");
    // 5) 设置页不回绕;恢复出厂在最后(返回之后)
    assert(SET_RESET == SET_COUNT - 1 && SET_BACK == SET_RESET - 1);
    assert(pet_set_step(0, -1, SET_COUNT) == 0);                  // 第一项往上:停住,不会跳到恢复出厂
    assert(pet_set_step(SET_COUNT - 1, 1, SET_COUNT) == SET_COUNT - 1);
    int s = 0; for (int i = 0; i < 10; i++) s = pet_set_step(s, -1, SET_COUNT); assert(s == 0);
    s = 0; s = pet_set_step(s, 1, SET_COUNT); s = pet_set_step(s, 1, SET_COUNT); assert(s == SET_BACK);
    s = pet_set_step(s, 1, SET_COUNT); assert(s == SET_RESET);
    puts("settings menu does not wrap: PASS");
    puts("test_reset_v453: OK");
    return 0;
}
