// tests/test_ams_v43.c — v4.3 耗材页:AMS 解析(含真实 H2D push_all)、HT 识别、空/余量低/在用、翻页到边切机器。
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
static void feed(bambu_parse_ctx_t *c, printer_status_t *st, pet_ams_t *a, const char *json) {
    cJSON *r = cJSON_Parse(json); assert(r);
    const cJSON *p = cJSON_GetObjectItem(r, "print"); assert(p);
    bambu_parse_print(c, p, st); bambu_parse_ams(c, p, a);
    cJSON_Delete(r);
}
static const pet_tray_t *slot(const pet_ams_t *a, int id, int s) {
    for (int u = 0; u < a->n_units; u++) if (a->u[u].id == id) { assert(s < a->u[u].nslot); return &a->slot[a->u[u].first + s]; }
    assert(!"unit not found"); return NULL;
}
#define EMPTY(t) (((t)->flags & PET_TRAY_EMPTY) != 0)
#define ACTIVE(t) (((t)->flags & PET_TRAY_ACTIVE) != 0)

static void test_h2d(const char *path) {
    char *raw = slurp(path);
    cJSON *root = cJSON_Parse(raw); assert(root);
    const cJSON *p = cJSON_GetObjectItem(root, "push_all");
    printer_status_t st; printer_status_reset(&st);
    bambu_parse_ctx_t c = BAMBU_PARSE_CTX_INIT;
    pet_ams_t a; pet_ams_reset(&a);
    bambu_parse_print(&c, p, &st); bambu_parse_ams(&c, p, &a);
    char nm[8], hm[8];
    for (int u = 0; u < a.n_units; u++) {
        pet_ams_unit_name(a.u[u].id, nm, sizeof nm); pet_ams_humidity_text(&a.u[u], hm, sizeof hm);
        printf("H2D %-6s %-4s:", nm, hm);
        for (int s = 0; s < a.u[u].nslot; s++) {
            const pet_tray_t *t = &a.slot[a.u[u].first + s];
            printf(" [%s %02X%02X%02X r%d%s%s%s]", EMPTY(t) ? "空" : t->type, t->r, t->g, t->b, t->remain,
                   pet_tray_low(t) ? " low" : "", ACTIVE(t) ? " *" : "", pet_tray_dark(t) ? " dark" : "");
        }
        printf("\n");
    }
    for (int e = 0; e < a.n_ext; e++) printf("H2D ext %s %s\n", (a.ext[e].flags & PET_TRAY_SIDE_L) ? "左" : "右", EMPTY(&a.ext[e]) ? "未设置" : a.ext[e].type);
    assert(a.valid && a.n_units == 2 && a.n_slots == 5);
    assert(a.u[0].id == 0 && a.u[1].id == 128 && pet_ams_is_ht(a.u[1].id) && a.u[1].nslot == 1);
    assert(a.u[0].hum_pct == 24 && a.u[0].hum_lvl == 4);
    assert(a.u[1].hum_pct == 6 && a.u[1].hum_lvl == -1);       // humidity "0" 不是有效等级
    pet_ams_humidity_text(&a.u[0], hm, sizeof hm); assert(!strcmp(hm, "24%"));
    const pet_tray_t *t = slot(&a, 0, 0); assert(!EMPTY(t) && !strcmp(t->type, "PLA") && t->r == 0xFF && t->remain == 47 && !pet_tray_dark(t));
    t = slot(&a, 0, 1); assert(!strcmp(t->type, "PLA") && t->r <= 1 && t->remain == 35 && pet_tray_dark(t));
    t = slot(&a, 0, 2); assert(t->r == 0xDE && t->g == 0x43 && t->remain == 82);
    t = slot(&a, 0, 3); assert(!strcmp(t->type, "PETG") && t->remain == 26 && !pet_tray_low(t));
    t = slot(&a, 128, 0); assert(!strcmp(t->type, "PA-GF") && t->remain == 100);
    // 在用:右喷嘴 snow 0x8000 = HT-A;左喷嘴 0xFEFF = 没有料
    assert(ACTIVE(slot(&a, 128, 0)));
    for (int s = 0; s < 4; s++) assert(!ACTIVE(slot(&a, 0, s)));
    // 外挂:vir_slot 254 = 左,255 = 右,都没设置
    assert(a.n_ext == 2 && (a.ext[0].flags & PET_TRAY_SIDE_L) && (a.ext[1].flags & PET_TRAY_SIDE_R));
    assert(EMPTY(&a.ext[0]) && EMPTY(&a.ext[1]) && !ACTIVE(&a.ext[0]));
    assert(pet_ams_row_count(&a) == 3 && pet_ams_scroll(&a, 0, 1, PET_AMS_VIEW_H) == -1);
    cJSON_Delete(root); free(raw);
    printf("H2D sample: PASS\n");
}

static void test_single_nozzle(void) {
    printer_status_t st; printer_status_reset(&st);
    bambu_parse_ctx_t c = BAMBU_PARSE_CTX_INIT;
    pet_ams_t a; pet_ams_reset(&a);
    feed(&c, &st, &a, "{\"print\":{\"nozzle_temper\":30,\"ams\":{\"tray_now\":\"6\",\"tray_exist_bits\":\"7f\",\"ams\":["
        "{\"id\":\"1\",\"humidity\":\"3\",\"tray\":[{\"id\":\"0\",\"tray_type\":\"PLA\",\"tray_color\":\"FF0000FF\",\"remain\":10},"
        "{\"id\":\"1\",\"tray_type\":\"\",\"tray_color\":\"00000000\"},{\"id\":\"2\",\"tray_type\":\"PETG-HF\",\"tray_color\":\"161616FF\",\"remain\":-1},{\"id\":\"3\"}]},"
        "{\"id\":\"0\",\"humidity\":\"5\",\"humidity_raw\":\"39\",\"tray\":[{\"id\":\"0\",\"tray_type\":\"PLA\",\"tray_color\":\"00AE42FF\",\"remain\":0},"
        "{\"id\":\"1\",\"tray_type\":\"PLA\",\"tray_color\":\"F5B6CDFF\",\"remain\":16},{\"id\":\"2\",\"tray_type\":\"PLA\",\"tray_color\":\"8E9089FF\",\"remain\":15},"
        "{\"id\":\"3\",\"tray_type\":\"PLA\",\"tray_color\":\"F4E4D6FF\",\"remain\":100}]}]},"
        "\"vt_tray\":{\"id\":\"255\",\"tray_type\":\"\",\"tray_color\":\"00000000\",\"remain\":0}}}");
    assert(a.n_units == 2 && a.u[0].id == 0 && a.u[1].id == 1);   // 按 id 排序
    char hm[8];
    pet_ams_humidity_text(&a.u[0], hm, sizeof hm); assert(!strcmp(hm, "39%"));
    pet_ams_humidity_text(&a.u[1], hm, sizeof hm); assert(!strcmp(hm, "3级"));   // 没有 humidity_raw -> 等级
    // 余量红点:0 和 15 有,16 没有,-1 没有,空槽没有
    assert(pet_tray_low(slot(&a, 0, 0)) && !pet_tray_low(slot(&a, 0, 1)) && pet_tray_low(slot(&a, 0, 2)));
    assert(pet_tray_low(slot(&a, 1, 0)));
    assert(EMPTY(slot(&a, 1, 1)) && !pet_tray_low(slot(&a, 1, 1)));            // tray_type "" = 空
    assert(!EMPTY(slot(&a, 1, 2)) && !pet_tray_low(slot(&a, 1, 2)) && pet_tray_dark(slot(&a, 1, 2)));
    assert(!strcmp(slot(&a, 1, 2)->type, "PETG-HF"));
    assert(EMPTY(slot(&a, 1, 3)));                                              // 只有 id(且 exist bit 7 = 0)
    assert(ACTIVE(slot(&a, 1, 2)) && !strcmp(pet_tray_tag(slot(&a, 1, 2)), ""));   // tray_now 6 = B3;单喷嘴:无字母
    int act = 0; for (int i = 0; i < a.n_slots; i++) act += ACTIVE(&a.slot[i]) ? 1 : 0;
    assert(act == 1);
    assert(a.n_ext == 1 && EMPTY(&a.ext[0]) && !(a.ext[0].flags & (PET_TRAY_SIDE_L | PET_TRAY_SIDE_R)));
    // 增量:只有 tray_now -> 机组不变,在用改为 A2
    feed(&c, &st, &a, "{\"print\":{\"ams\":{\"tray_now\":\"1\"}}}");
    assert(a.n_units == 2 && ACTIVE(slot(&a, 0, 1)) && !ACTIVE(slot(&a, 1, 2)));
    // 增量:只有 state 的料槽对象保留类型;state 无料盘 -> 空
    feed(&c, &st, &a, "{\"print\":{\"ams\":{\"ams\":[{\"id\":\"0\",\"tray\":[{\"id\":\"3\",\"state\":11},{\"id\":\"2\",\"state\":0}]},{\"id\":\"1\"}]}}}");
    assert(!EMPTY(slot(&a, 0, 3)) && !strcmp(slot(&a, 0, 3)->type, "PLA"));
    assert(EMPTY(slot(&a, 0, 2)) && !pet_tray_low(slot(&a, 0, 2)));
    assert(!strcmp(slot(&a, 1, 2)->type, "PETG-HF"));                           // 没出现的机组数据保留
    // tray_exist_bits 清零 -> 空
    feed(&c, &st, &a, "{\"print\":{\"ams\":{\"tray_exist_bits\":\"7e\"}}}");
    assert(EMPTY(slot(&a, 0, 0)));
    // 外挂在用 + 设置了
    feed(&c, &st, &a, "{\"print\":{\"ams\":{\"tray_now\":\"254\"},\"vt_tray\":{\"id\":\"255\",\"tray_type\":\"TPU\",\"tray_color\":\"FFFFFFFF\"}}}");
    assert(!EMPTY(&a.ext[0]) && !strcmp(a.ext[0].type, "TPU") && ACTIVE(&a.ext[0]) && a.ext[0].remain == -1);
    for (int i = 0; i < a.n_slots; i++) assert(!ACTIVE(&a.slot[i]));
    // 机组被拔掉:ams[] 里没有了就去掉
    feed(&c, &st, &a, "{\"print\":{\"ams\":{\"ams\":[{\"id\":\"1\"}]}}}");
    assert(a.n_units == 1 && a.u[0].id == 1 && a.u[0].first == 0 && !strcmp(a.slot[2].type, "PETG-HF"));
    printf("single-nozzle / incremental: PASS\n");
}

static void test_h2_both_nozzles(void) {
    printer_status_t st; printer_status_reset(&st);
    bambu_parse_ctx_t c = BAMBU_PARSE_CTX_INIT;
    pet_ams_t a; pet_ams_reset(&a);
    feed(&c, &st, &a, "{\"print\":{\"device\":{\"extruder\":{\"state\":18,\"info\":[{\"id\":0,\"snow\":32768},{\"id\":1,\"snow\":258}]}},"
        "\"ams\":{\"ams\":[{\"id\":\"128\",\"humidity_raw\":\"22\",\"tray\":[{\"id\":\"0\",\"tray_type\":\"PLA\",\"tray_color\":\"FFFFFFFF\",\"state\":11}]},"
        "{\"id\":\"1\",\"tray\":[{\"id\":\"0\",\"tray_type\":\"PLA\",\"tray_color\":\"000000FF\",\"state\":11},{\"id\":\"1\",\"state\":11,\"tray_type\":\"PLA\"},"
        "{\"id\":\"2\",\"tray_type\":\"PLA\",\"tray_color\":\"C12E1FFF\",\"state\":11,\"remain\":10},{\"id\":\"3\",\"state\":0}]}]},"
        "\"vir_slot\":[{\"id\":\"255\",\"tray_type\":\"\"},{\"id\":\"254\",\"tray_type\":\"PLA\",\"tray_color\":\"76D9F4FF\"}]}}");
    assert(a.n_units == 2 && a.u[0].id == 1 && a.u[1].id == 128);              // HT 排在普通 AMS 后面
    assert(ACTIVE(slot(&a, 128, 0)) && ACTIVE(slot(&a, 1, 2)));                // 两个喷嘴各一个在用
    assert(!strcmp(pet_tray_tag(slot(&a, 128, 0)), "R") && !strcmp(pet_tray_tag(slot(&a, 1, 2)), "L"));   // id0 = 右, id1 = 左
    assert(pet_tray_low(slot(&a, 1, 2)) && EMPTY(slot(&a, 1, 3)));
    assert(a.n_ext == 2 && (a.ext[0].flags & PET_TRAY_SIDE_L) && !EMPTY(&a.ext[0]) && EMPTY(&a.ext[1]));
    // 左喷嘴换到外挂(254 << 8 | 0)-> 只有左外挂在用
    feed(&c, &st, &a, "{\"print\":{\"device\":{\"extruder\":{\"info\":[{\"id\":1,\"snow\":65024}]}}}}");
    assert(ACTIVE(&a.ext[0]) && !ACTIVE(&a.ext[1]) && !ACTIVE(slot(&a, 1, 2)) && ACTIVE(slot(&a, 128, 0)));
    // 两个喷嘴用同一个槽 -> 一个框,标签 "LR"
    feed(&c, &st, &a, "{\"print\":{\"device\":{\"extruder\":{\"info\":[{\"id\":1,\"snow\":32768}]}}}}");
    assert(!strcmp(pet_tray_tag(slot(&a, 128, 0)), "LR") && !ACTIVE(&a.ext[0]));
    printf("H2 both nozzles / vir_slot sides: PASS\n");
}

static void test_names_scroll(void) {
    char b[8];
    pet_ams_unit_name(0, b, sizeof b); assert(!strcmp(b, "AMS-A"));
    pet_ams_unit_name(3, b, sizeof b); assert(!strcmp(b, "AMS-D"));
    pet_ams_unit_name(128, b, sizeof b); assert(!strcmp(b, "HT-A"));
    pet_ams_unit_name(135, b, sizeof b); assert(!strcmp(b, "HT-H"));
    assert(pet_ams_is_ht(128) && pet_ams_is_ht(135) && !pet_ams_is_ht(136) && !pet_ams_is_ht(3));
    // 演示数据:AMS-A/B + HT-A + 外挂 = 56+6+56+6+56+6+28 = 214,正好一屏
    pet_ams_t k; pet_fake_ams(0, &k);
    assert(k.n_units == 3 && k.n_slots == 9 && pet_ams_row_count(&k) == 4);
    assert(pet_ams_rows_fit(&k, 0, PET_AMS_VIEW_H) == 4);
    assert(pet_ams_scroll(&k, 0, 1, PET_AMS_VIEW_H) == -1 && pet_ams_scroll(&k, 0, -1, PET_AMS_VIEW_H) == -1);
    assert(ACTIVE(&k.slot[8]) && ACTIVE(&k.slot[0]) && EMPTY(&k.slot[7]) && pet_tray_low(&k.slot[6]));
    assert(!strcmp(pet_tray_tag(&k.slot[0]), "L") && !strcmp(pet_tray_tag(&k.slot[8]), "R") && !strcmp(pet_tray_tag(&k.slot[1]), ""));
    int act = 0, low = 0; for (int i = 0; i < k.n_slots; i++) { act += ACTIVE(&k.slot[i]); low += pet_tray_low(&k.slot[i]); }
    assert(act == 2 && low == 1);
    assert(k.u[0].hum_pct == 39 && k.u[1].hum_pct == 46 && k.u[2].hum_pct == 22 && k.u[2].id == 128);
    assert(k.n_ext == 1 && (k.ext[0].flags & PET_TRAY_SIDE_L) && EMPTY(&k.ext[0]));
    // 溢出:4 台 AMS + 2 台 HT + 外挂 = 7 行
    pet_ams_t o; pet_ams_reset(&o);
    int ids[6] = { 0, 1, 2, 3, 128, 129 };
    for (int i = 0; i < 6; i++) { int ns = i < 4 ? 4 : 1; o.u[i] = (pet_ams_unit_t){ (uint8_t)ids[i], -1, -1, o.n_slots, (uint8_t)ns }; o.n_slots += ns; }
    o.n_units = 6; o.n_ext = 2;
    assert(pet_ams_row_count(&o) == 7 && pet_ams_rows_fit(&o, 0, PET_AMS_VIEW_H) == 3);
    int f = 0, steps = 0;
    while ((f = pet_ams_scroll(&o, f, 1, PET_AMS_VIEW_H)) >= 0) { steps++; assert(f == steps); if (steps > 10) break; }
    assert(steps == 3);                                    // 0 -> 1 -> 2 -> 3,第 4 次到底(该切机器)
    assert(pet_ams_rows_fit(&o, 3, PET_AMS_VIEW_H) == 4);  // 3,4,5 + 外挂
    f = 3; steps = 0;
    while ((f = pet_ams_scroll(&o, f, -1, PET_AMS_VIEW_H)) >= 0) steps++;
    assert(steps == 3);                                    // 回到顶,再按上 = 切机器
    // 上限:13 台机组只收 12 台、槽位不超过 24
    char js[2048]; int n = snprintf(js, sizeof js, "{\"print\":{\"ams\":{\"ams\":[");
    for (int i = 0; i < 13; i++) n += snprintf(js + n, sizeof js - n, "%s{\"id\":\"%d\"}", i ? "," : "", i < 4 ? i : 124 + i);
    snprintf(js + n, sizeof js - n, "]}}}");
    printer_status_t st; printer_status_reset(&st); bambu_parse_ctx_t c = BAMBU_PARSE_CTX_INIT; pet_ams_t a; pet_ams_reset(&a);
    feed(&c, &st, &a, js);
    assert(a.n_units <= PET_AMS_MAX_UNITS && a.n_slots <= PET_AMS_MAX_SLOTS);
    printf("names / scroll / overflow / demo: PASS (sizeof pet_ams_t = %zu B)\n", sizeof(pet_ams_t));
}

static void test_ext_count(void) {
    // 外挂行数 = 报文里实际有几项:没有就 0,vt_tray 1 项,vir_slot 1 项/2 项
    printer_status_t st; bambu_parse_ctx_t c; pet_ams_t a;
    printer_status_reset(&st); c = (bambu_parse_ctx_t)BAMBU_PARSE_CTX_INIT; pet_ams_reset(&a);
    feed(&c, &st, &a, "{\"print\":{\"ams\":{\"ams\":[{\"id\":\"0\"}]}}}");
    assert(a.n_ext == 0 && pet_ams_row_count(&a) == 1);
    feed(&c, &st, &a, "{\"print\":{\"vt_tray\":{\"id\":\"255\",\"tray_type\":\"\"}}}");
    assert(a.n_ext == 1 && pet_ams_row_count(&a) == 2);
    printer_status_reset(&st); c = (bambu_parse_ctx_t)BAMBU_PARSE_CTX_INIT; pet_ams_reset(&a);
    feed(&c, &st, &a, "{\"print\":{\"vir_slot\":[{\"id\":\"254\",\"tray_type\":\"\"}]}}");
    assert(a.n_ext == 1 && (a.ext[0].flags & PET_TRAY_SIDE_L));
    feed(&c, &st, &a, "{\"print\":{\"vir_slot\":[{\"id\":\"254\"},{\"id\":\"255\",\"tray_type\":\"PLA\"}]}}");
    assert(a.n_ext == 2 && (a.ext[1].flags & PET_TRAY_SIDE_R) && !(a.ext[1].flags & PET_TRAY_EMPTY));
    // 单喷嘴:在用槽没有标签(UI 只画绿框)
    pet_ams_t p; pet_fake_ams(1, &p);
    int act = 0; for (int i = 0; i < p.n_slots; i++) if (p.slot[i].flags & PET_TRAY_ACTIVE) { act++; assert(!strcmp(pet_tray_tag(&p.slot[i]), "")); }
    assert(act == 1);
    printf("external count / single-nozzle no tag: PASS\n");
}

int main(int argc, char **argv) {
    test_h2d(argc > 1 ? argv[1] : "tests/fixtures/H2D_ha-bambulab.json");
    test_single_nozzle();
    test_h2_both_nozzles();
    test_names_scroll();
    test_ext_count();
    printf("test_ams_v43: PASS\n");
    return 0;
}
