// tests/test_stream_v443.c — v4.4.3:MQTT 分片流式过滤。整条解析 vs 分片过滤后解析,printer_status/AMS 必须完全一致。
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "bambu_parse.h"
#include "json_filter.h"
#include "pet_logic.h"

static char *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); assert(f); fseek(f, 0, SEEK_END); long l = ftell(f); rewind(f);
    char *b = malloc(l + 1); assert(fread(b, 1, l, f) == (size_t)l); b[l] = 0; fclose(f); *n = l; return b;
}
static void parse(const char *json, size_t n, printer_status_t *st, pet_ams_t *a) {
    cJSON *r = cJSON_ParseWithLength(json, n); assert(r);
    const cJSON *p = cJSON_GetObjectItem(r, "print"); assert(p);
    bambu_parse_ctx_t c = BAMBU_PARSE_CTX_INIT;
    printer_status_reset(st); pet_ams_reset(a);
    bambu_parse_print(&c, p, st); bambu_parse_ams(&c, p, a);
    cJSON_Delete(r);
}
static char out[8192];
static size_t run(const char *raw, size_t n, size_t frag, printer_status_t *st, pet_ams_t *a) {
    jf_t f; jf_init(&f, out, sizeof out, JF_BAMBU_KEYS, JF_BAMBU_N_KEYS);
    unsigned seed = (unsigned)frag * 2654435761u;
    for (size_t o = 0; o < n; ) {           // frag==0:随机分片 1..4096
        size_t k = frag ? frag : 1 + ((seed = seed * 1103515245u + 12345u) >> 8) % 4096;
        if (k > n - o) k = n - o;
        jf_feed(&f, raw + o, k); o += k;
    }
    assert(jf_finish(&f));
    assert(f.in_total == n);
    parse(f.out, f.len, st, a);
    return f.len;
}
static void check(const char *path) {
    size_t n; char *raw = slurp(path, &n);
    printer_status_t s0, s1; pet_ams_t a0, a1;
    parse(raw, n, &s0, &a0);
    assert(s0.n_nozzles > 0);
    size_t frags[] = { 1, 7, 100, 1024, 4096 - 64, 4096, 0, 0, 0, 1 << 20 };
    size_t kept = 0;
    for (size_t i = 0; i < sizeof frags / sizeof frags[0]; i++) {
        kept = run(raw, n, frags[i] ? frags[i] : 1000 + i, &s1, &a1);
        if (!frags[i]) kept = run(raw, n, 0, &s1, &a1);
        assert(!memcmp(&s0, &s1, sizeof s0));
        assert(!memcmp(&a0, &a1, sizeof a0));
    }
    printf("stream %-26s raw=%6zu B kept=%5zu B ams_units=%d nozzles=%d OK\n", strrchr(path, '/') + 1, n, kept, a0.n_units, s0.n_nozzles);
    free(raw);
}
static void bad(const char *s, size_t cap) {
    char o[64]; jf_t f; jf_init(&f, o, cap, JF_BAMBU_KEYS, JF_BAMBU_N_KEYS);
    jf_feed(&f, s, strlen(s)); assert(!jf_finish(&f));
}
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) check(argv[i]);
    bad("{\"print\":{\"mc_percent\":5", 64);           // 截断
    bad("{\"print\":{\"mc_percent\":5]}", 64);         // 括号不配
    bad("{\"print\":{\"subtask_name\":\"0123456789012345678901234567890123456789012345678901234567890\"}}", 64);   // 输出溢出
    bad("", 64);
    { char o[64]; jf_t f; jf_init(&f, o, sizeof o, JF_BAMBU_KEYS, JF_BAMBU_N_KEYS);
      const char *s = " { \"x\" : [1,{\"print\":2}], \"print\" : { \"mc_percent\" : 5 , \"y\":\"a\\\"}\" , \"ams\":[ ] } } ";
      jf_feed(&f, s, strlen(s)); assert(jf_finish(&f)); assert(!strcmp(o, "{\"print\":{\"mc_percent\":5,\"ams\":[]}}")); }
    {   // 「读不到打印机数据」vs「未连上拓竹云」
        pet_conn_t k; pet_conn_init(&k);
        assert(pet_conn_update_ex(&k, false, false, false, 0) == PET_CONN_CONNECTING);
        assert(pet_conn_update_ex(&k, false, true, false, 29999) == PET_CONN_CONNECTING);   // 连上了,还在等
        assert(pet_conn_update_ex(&k, false, true, true, 5000) == PET_CONN_NODATA);         // 连上 + 解析失败:立刻
        assert(pet_conn_update_ex(&k, false, true, false, 30000) == PET_CONN_NODATA);       // 连上 30 秒没温度
        assert(pet_conn_update_ex(&k, false, false, false, 30000) == PET_CONN_FAILED);      // Wi-Fi/列表/MQTT 失败
        assert(pet_conn_update_ex(&k, true, true, false, 31000) == PET_CONN_DATA);          // 第一条完整数据立刻回正常
        assert(pet_conn_update_ex(&k, false, true, false, 32000) == PET_CONN_CONNECTING);
    }
    puts("test_stream_v443: OK");
    return 0;
}
