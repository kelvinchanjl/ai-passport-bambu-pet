// tests/test_v441.c — v4.4.1:详情页「当前/目标」温度、H2 双喷嘴、没数据时 30 秒「连接中→未连上」、第一条报文立刻切。
// 注意:真机日志里没有报文原文,下面的 H2C 报文是按同一时刻 Handy 截图的数值
// 手写的 H2 结构(device.extruder.info[].temp / device.bed.info.temp / device.ctc.info.temp 打包:低 16 位当前,高 16 位目标),
// 不是真机抓到的原文。真机原文要等 v4.4.1 测试版的 payload 日志。
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "bambu_parse.h"
static const char *H2C_HANDY =
  "{\"print\":{\"gcode_state\":\"FINISH\",\"mc_percent\":100,\"layer_num\":8,\"total_layer_num\":8,\"mc_remaining_time\":0,"
  "\"subtask_name\":\"GPT White over Green - calibration\","
  "\"device\":{\"extruder\":{\"state\":2,\"info\":[{\"id\":0,\"temp\":40,\"snow\":65280},{\"id\":1,\"temp\":36,\"snow\":65535}]},"
  "\"bed\":{\"info\":{\"temp\":33}},\"ctc\":{\"info\":{\"temp\":34}}}}}";
int main(void) {
    char c[12], t[16];
    // --- 当前/目标:空闲目标 0 也显示,当前值绝不因为目标 0 变成 0 ---
    pet_temp_pair(true, 30.2f, 0, true, c, sizeof c, t, sizeof t); assert(!strcmp(c, "30") && !strcmp(t, "/0°C"));
    pet_temp_pair(true, 218, 220, true, c, sizeof c, t, sizeof t); assert(!strcmp(c, "218") && !strcmp(t, "/220°C"));
    pet_temp_pair(true, 350, 350, true, c, sizeof c, t, sizeof t); assert(!strcmp(c, "350") && !strcmp(t, "/350°C"));
    pet_temp_pair(true, 27, 0, false, c, sizeof c, t, sizeof t);   assert(!strcmp(c, "27") && !strcmp(t, "/--"));   // 仓温没有目标
    pet_temp_pair(false, 0, 0, true, c, sizeof c, t, sizeof t);    assert(!strcmp(c, "--") && !strcmp(t, ""));      // 没数据
    // --- 双喷嘴 ---
    assert(pet_is_dual(2, "P1S") && pet_is_dual(0, "H2C") && pet_is_dual(0, "H2D") && !pet_is_dual(1, "P1S") && !pet_is_dual(0, "") && !pet_is_dual(0, NULL));
    // --- H2C 结构(Handy 同一时刻:L 36/0、R 40/0、热床 33/0、仓温 34/0、100%、层 8/8)---
    bambu_parse_ctx_t ctx = BAMBU_PARSE_CTX_INIT; printer_status_t st; printer_status_reset(&st);
    cJSON *r = cJSON_Parse(H2C_HANDY); assert(r);
    bambu_parse_print(&ctx, cJSON_GetObjectItem(r, "print"), &st); cJSON_Delete(r);
    assert(st.valid && st.n_nozzles == 2);
    pet_temp_pair(st.valid, st.noz[0], st.noz_t[0], true, c, sizeof c, t, sizeof t); assert(!strcmp(c, "36") && !strcmp(t, "/0°C"));   // L = extruder id 1
    pet_temp_pair(st.valid, st.noz[1], st.noz_t[1], true, c, sizeof c, t, sizeof t); assert(!strcmp(c, "40") && !strcmp(t, "/0°C"));   // R = extruder id 0
    pet_temp_pair(st.valid, st.bed, st.bed_target, true, c, sizeof c, t, sizeof t); assert(!strcmp(c, "33") && !strcmp(t, "/0°C"));
    assert(st.has_chamber && st.chamber_has_target);
    pet_temp_pair(st.valid, st.chamber, st.chamber_target, st.chamber_has_target, c, sizeof c, t, sizeof t); assert(!strcmp(c, "34") && !strcmp(t, "/0°C"));
    assert(st.percent == 100 && st.layer == 8 && st.total_layer == 8 && !strcmp(st.subtask, "GPT White over Green - calibration"));
    // packed with a target: 220 << 16 | 218
    r = cJSON_Parse("{\"print\":{\"device\":{\"bed\":{\"info\":{\"temp\":4325441}}}}}"); assert(r);   // 66<<16 | 65
    bambu_parse_print(&ctx, cJSON_GetObjectItem(r, "print"), &st); cJSON_Delete(r);
    assert(st.bed == 65 && st.bed_target == 66);
    // --- 连接中 30 秒 → 未连上;第一条数据立刻切 ---
    pet_conn_t k; pet_conn_init(&k);
    assert(pet_conn_update(&k, false, 1000) == PET_CONN_CONNECTING);
    assert(pet_conn_update(&k, false, 1000 + 29999) == PET_CONN_CONNECTING);
    assert(pet_conn_update(&k, false, 1000 + 30000) == PET_CONN_FAILED);
    assert(pet_conn_update(&k, true, 45000) == PET_CONN_DATA);              // 第一条数据:立刻有数据(包括已经「未连上」时)
    assert(pet_conn_update(&k, false, 50000) == PET_CONN_CONNECTING);       // 数据丢了:重新计 30 秒
    assert(pet_conn_update(&k, false, 79999) == PET_CONN_CONNECTING);
    assert(pet_conn_update(&k, false, 80000) == PET_CONN_FAILED);
    pet_conn_init(&k); assert(pet_conn_update(&k, true, 5) == PET_CONN_DATA);   // 开机就有数据:从不显示连接中
    pet_conn_init(&k); assert(pet_conn_update(&k, false, 0xFFFFF000u) == PET_CONN_CONNECTING);
    assert(pet_conn_update(&k, false, 0xFFFFF000u + 30000u) == PET_CONN_FAILED);   // 计时回绕
    puts("test_v441: PASS");
    return 0;
}
