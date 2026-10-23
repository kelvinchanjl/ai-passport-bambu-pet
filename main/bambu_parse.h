// main/bambu_parse.h — 把一条 report 里的 "print" 对象合并进 printer_status_t(增量报文只改出现的字段)
#pragma once
#include "cJSON.h"
#include "pet_logic.h"
typedef struct {
    int tray_now; int active_nozzle;      // 跨报文保留的状态,初始 {-1, 0};active_nozzle 是拓竹 extruder id(H2: 0 = 右, 1 = 左)
    int snow[2];                           // H2:各喷嘴当前料槽 (ams_id<<8 | slot),-1 = 未知
    bool h2;                               // 见过 device.extruder.info[] 两项
    bool has_exist; uint32_t exist_bits;   // v4.3:ams.tray_exist_bits(bit = ams*4+slot;HT 128.. = 16+)
} bambu_parse_ctx_t;
#define BAMBU_PARSE_CTX_INIT { .tray_now = -1, .active_nozzle = 0, .snow = { -1, -1 }, .h2 = false, .has_exist = false, .exist_bits = 0 }
void bambu_parse_print(bambu_parse_ctx_t *ctx, const cJSON *print, printer_status_t *st);
// v4.3 耗材页:把 print.ams / vt_tray / vir_slot 合并进 a(增量报文只改出现的字段),再按 snow / tray_now 标在用槽。
// 在 bambu_parse_print() 之后调用(要用它更新过的 ctx->snow / tray_now / h2)。
void bambu_parse_ams(bambu_parse_ctx_t *ctx, const cJSON *print, pet_ams_t *a);
