// tests/test_fields_v42.c — v4.2 字段核对:用真实 H2D push_all(ha-bambulab 采集)喂真实解析器,逐项断言详情页字段。
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "bambu_parse.h"

static char *slurp(const char *p) {
    FILE *f = fopen(p, "rb"); assert(f); fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    char *b = malloc(n + 1); assert(fread(b, 1, n, f) == (size_t)n); b[n] = 0; fclose(f); return b;
}
int main(int argc, char **argv) {
    char *raw = slurp(argc > 1 ? argv[1] : "tests/fixtures/H2D_ha-bambulab.json");
    cJSON *root = cJSON_Parse(raw); assert(root);
    const cJSON *p = cJSON_GetObjectItem(root, "push_all"); if (!p) p = cJSON_GetObjectItem(root, "print");
    printer_status_t st; printer_status_reset(&st);
    bambu_parse_ctx_t ctx = BAMBU_PARSE_CTX_INIT;
    bambu_parse_print(&ctx, p, &st);
    char t[24];
    printf("subtask_name      = %s\n", st.subtask);
    printf("mc_percent        = %d\n", st.percent);
    printf("layer             = %d/%d\n", st.layer, st.total_layer);
    printf("mc_remaining_time = %d\n", st.remaining_min);
    printf("n_nozzles=%d active(UI)=%d\n", st.n_nozzles, st.active_nozzle);
    pet_format_temp(st.noz[0], st.noz_t[0], t, sizeof t); printf("左喷嘴(id1)       = %s (raw %.0f/%.0f)\n", t, st.noz[0], st.noz_t[0]);
    pet_format_temp(st.noz[1], st.noz_t[1], t, sizeof t); printf("右喷嘴(id0)       = %s (raw %.0f/%.0f)\n", t, st.noz[1], st.noz_t[1]);
    pet_format_temp(st.bed, st.bed_target, t, sizeof t);  printf("热床              = %s\n", t);
    pet_format_temp(st.chamber, st.chamber_target, t, sizeof t); printf("仓温(ctc)         = %s has=%d\n", t, st.has_chamber);
    printf("耗材              = %s · %s  %s 湿度 %d%%  rgb=%06X\n", st.fil_type, pet_color_name(st.fil_rgb), st.fil_slot, st.fil_humidity, (unsigned)st.fil_rgb);
    assert(!strcmp(st.subtask, "Number 10 House Numbers_plate_1"));
    assert(st.total_layer == 26 && st.layer == 0 && st.remaining_min == 115 && st.percent == 0);
    assert(st.n_nozzles == 2);
    assert(st.active_nozzle == 1);                         // extruder.state=2 -> id 0 -> 右
    assert(st.noz[1] == 264 && st.noz_t[1] == 225);        // id0 temp 14745864 = 0x00E1_0108
    assert(st.noz[0] == 40 && st.noz_t[0] == 0);           // id1 temp 40
    assert(st.bed == 100 && st.bed_target == 100);         // bed.info.temp 6553700 = 0x0064_0064
    assert(st.has_chamber && st.chamber == 48 && st.chamber_target == 60);   // ctc.info.temp 3932208 = 0x003C_0030
    assert(!strcmp(st.fil_slot, "HT-A"));                  // snow 32768 = 0x80 << 8 | 0
    assert(!strcmp(st.fil_type, "PA-GF") && st.fil_rgb == 0x010101 && st.fil_humidity == 6);
    pet_format_temp(st.noz[0], st.noz_t[0], t, sizeof t); assert(!strcmp(t, "40°C"));       // 目标 0:不出现 40/0°C
    pet_format_temp(st.noz[1], st.noz_t[1], t, sizeof t); assert(!strcmp(t, "264°C"));      // 高于目标:只显示当前
    // 单喷嘴(P1S 风格)增量报文
    printer_status_t s2; printer_status_reset(&s2); bambu_parse_ctx_t c2 = BAMBU_PARSE_CTX_INIT;
    cJSON *m = cJSON_Parse("{\"nozzle_temper\":219.6,\"nozzle_target_temper\":220,\"bed_temper\":55,\"ams\":{\"tray_now\":\"1\",\"ams\":[{\"id\":\"0\",\"humidity_raw\":\"31\",\"tray\":[{\"id\":\"1\",\"tray_type\":\"PETG\",\"tray_color\":\"3C78E6FF\"}]}]}}");
    bambu_parse_print(&c2, m, &s2); cJSON_Delete(m);
    assert(s2.n_nozzles == 1 && s2.noz[0] > 219 && !s2.has_chamber);
    assert(!strcmp(s2.fil_slot, "A2") && !strcmp(s2.fil_type, "PETG") && s2.fil_humidity == 31);
    assert(!strcmp(pet_color_name(s2.fil_rgb), "蓝色"));
    // H2: snow 0xFEFF = 没有料(勿标外挂);0xFE00 = 左外挂;0xFF00 = 右外挂
    printer_status_t s3; printer_status_reset(&s3); bambu_parse_ctx_t c3 = BAMBU_PARSE_CTX_INIT;
    cJSON *h = cJSON_Parse("{\"device\":{\"extruder\":{\"state\":18,\"info\":[{\"id\":0,\"snow\":32768},{\"id\":1,\"snow\":65279}]}},\"ams\":{\"ams\":[{\"id\":\"128\",\"humidity_raw\":\"22\",\"tray\":[{\"id\":\"0\",\"tray_type\":\"PLA\",\"tray_color\":\"FFFFFFFF\"}]}]},\"vir_slot\":[{\"id\":\"254\",\"tray_type\":\"PLA\",\"tray_color\":\"010101FF\"},{\"id\":\"255\",\"tray_type\":\"TPU\",\"tray_color\":\"00AE42FF\"}]}");
    assert(h);
    bambu_parse_print(&c3, h, &s3); cJSON_Delete(h);
    assert(s3.active_nozzle == 0);                         // state 18 -> id 1 -> UI 左
    assert(s3.fil_slot[0] == 0);                           // 0xFEFF: 不写外挂,保持空
    // 左喷嘴切到真正外挂 0xFE00
    h = cJSON_Parse("{\"device\":{\"extruder\":{\"info\":[{\"id\":1,\"snow\":65024}]}},\"vir_slot\":[{\"id\":\"254\",\"tray_type\":\"PLA\",\"tray_color\":\"010101FF\"},{\"id\":\"255\",\"tray_type\":\"TPU\",\"tray_color\":\"00AE42FF\"}]}");
    assert(h);
    bambu_parse_print(&c3, h, &s3); cJSON_Delete(h);
    assert(!strcmp(s3.fil_slot, "外挂") && !strcmp(s3.fil_type, "PLA") && s3.fil_rgb == 0x010101);
    // 右喷嘴在用 + 右外挂 0xFF00
    printer_status_t s4; printer_status_reset(&s4); bambu_parse_ctx_t c4 = BAMBU_PARSE_CTX_INIT;
    h = cJSON_Parse("{\"device\":{\"extruder\":{\"state\":2,\"info\":[{\"id\":0,\"snow\":65280},{\"id\":1,\"snow\":65279}]}},\"vir_slot\":[{\"id\":\"254\",\"tray_type\":\"PLA\",\"tray_color\":\"010101FF\"},{\"id\":\"255\",\"tray_type\":\"TPU\",\"tray_color\":\"00AE42FF\"}]}");
    assert(h);
    bambu_parse_print(&c4, h, &s4); cJSON_Delete(h);
    assert(s4.active_nozzle == 1);                         // state 2 -> id 0 -> UI 右
    assert(!strcmp(s4.fil_slot, "外挂") && !strcmp(s4.fil_type, "TPU") && s4.fil_rgb == 0x00AE42);
    cJSON_Delete(root); free(raw);
    printf("field check: PASS\n");
    return 0;
}
