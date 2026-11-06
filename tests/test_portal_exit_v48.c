// v4.8:已配置工牌从设置进「重新配网」后可长按中键退出扫码页。
// 关热点、回到进配网前的页面、NVS 不回滚;首次配网和登录过期重新扫码不加退出。
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "pet_logic.h"

int main(void) {
    assert(!strcmp(PET_QR_EXIT_HINT, "长按中键 返回"));   // 半角空格,不用「退出」(字库不改)
    assert(strchr(PET_QR_EXIT_HINT, ' ') != NULL);
    assert(strstr(PET_QR_EXIT_HINT, "退出") == NULL);

    assert(pet_portal_exit_allowed(true, false));    // 已配置 + 设置→重新配网
    assert(!pet_portal_exit_allowed(false, false));  // 首次配网
    assert(!pet_portal_exit_allowed(true, true));    // 登录过期重新扫码
    assert(!pet_portal_exit_allowed(false, true));

    pet_portal_exit_plan_t p = pet_portal_exit_plan(true, false);
    assert(p.allow && p.stop_hotspot && p.restore_prev_page && !p.rollback_nvs);

    p = pet_portal_exit_plan(false, false);
    assert(!p.allow && !p.stop_hotspot && !p.restore_prev_page && !p.rollback_nvs);

    p = pet_portal_exit_plan(true, true);
    assert(!p.allow && !p.stop_hotspot && !p.restore_prev_page && !p.rollback_nvs);

    // 即使「手机已提交」(调用方 NVS 已写入),退出计划仍不回滚
    p = pet_portal_exit_plan(true, false);
    assert(!p.rollback_nvs);
    puts("portal exit allowed/plan (hotspot off, page restore, nvs kept): PASS");
    puts("test_portal_exit_v48 OK");
    return 0;
}
