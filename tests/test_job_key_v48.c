// v4.8:pet_job_key() 边界。原 main.c 的 job_key() 是 K[4] 直接下标,i>=4 越界读;
// 现在 0..PET_MAX_PRINTERS-1 返回固定键名(必须和 v4.7.x 写进 NVS "petjob" 的键完全一致,否则升级后用时记录丢失),越界返回 NULL。
#include "pet_logic.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    // 与 v4.7.x 已写入 NVS 的键名逐字一致
    static const char *const legacy[] = { "job", "job1", "job2", "job3" };
    assert(PET_MAX_PRINTERS == 4);   // 上限本版不变;改上限时本测试和 JOB_KEYS 一起改
    for (int i = 0; i < PET_MAX_PRINTERS; i++) {
        const char *k = pet_job_key(i);
        assert(k != NULL);
        assert(strcmp(k, legacy[i]) == 0);
        assert(strlen(k) <= 15);     // NVS 键名上限 15 字符
        for (int j = 0; j < i; j++) assert(strcmp(k, pet_job_key(j)) != 0);   // 互不相同
    }
    // 越界:返回 NULL,不能读到表外内存
    assert(pet_job_key(-1) == NULL);
    assert(pet_job_key(PET_MAX_PRINTERS) == NULL);
    assert(pet_job_key(PET_MAX_PRINTERS + 1) == NULL);
    assert(pet_job_key(255) == NULL);
    assert(pet_job_key(INT_MAX) == NULL);
    assert(pet_job_key(INT_MIN) == NULL);
    puts("test_job_key_v48 OK");
    return 0;
}
