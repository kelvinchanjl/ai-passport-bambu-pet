// v4.7 状态条纯函数:时间格式 + 电量平滑
#include "../main/status_bar.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
int main(void) {
    char b[8]; struct tm tm = {0}; tm.tm_hour = 9; tm.tm_min = 5;
    status_bar_fmt_time(&tm, true, b, sizeof b); assert(!strcmp(b, "09:05"));
    status_bar_fmt_time(&tm, false, b, sizeof b); assert(!strcmp(b, "--:--"));
    status_bar_fmt_time(NULL, true, b, sizeof b); assert(!strcmp(b, "--:--"));
    tm.tm_hour = 23; tm.tm_min = 59; status_bar_fmt_time(&tm, true, b, sizeof b); assert(!strcmp(b, "23:59"));
    assert(status_bar_smooth(-1, 80) == 80);
    assert(status_bar_smooth(80, -1) == 80);
    assert(status_bar_smooth(-1, -1) == -1);
    assert(status_bar_smooth(80, 101) == 80);
    int s = 80; for (int i = 0; i < 40; i++) s = status_bar_smooth(s, 60); assert(s == 60);
    s = 60; for (int i = 0; i < 40; i++) s = status_bar_smooth(s, 61); assert(s == 61);
    assert(status_bar_smooth(80, 79) == 79);
    puts("test_status_bar_v47 OK"); return 0;
}
