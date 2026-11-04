// v4.8 多组 Wi-Fi 的持久化:用内存里的假 NVS 跑真实的 main/app_config.c(app_config_wifi_load/save)。
// 覆盖:v4.7.x 单组 → 第 0 组迁移、重复开机不再写、旧键镜像(降级兼容)、旧固件改旧键后再升级、增删、恢复出厂、坏 blob、
// 未绑定设备、app-only 升级后 NVS 里所有旧键原样不动,以及 NVS 占用项数按 ESP-IDF 规则精确计数。
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvs.h"
#include "app_config.h"

// ---------------- 假 NVS ----------------
typedef struct { char ns[16], key[16]; char type; char *s; uint8_t *b; size_t blen; int64_t i; } item_t;
static item_t T[64]; static int N; static int WRITES;   // WRITES = 真正改变了存储内容的写次数(NVS 对相同值的 set 不写 flash)
static const char *g_ns;
static item_t *get(const char *k, char t) { for (int i = 0; i < N; i++) if (!strcmp(T[i].ns, g_ns) && !strcmp(T[i].key, k) && T[i].type == t) return &T[i]; return NULL; }
static item_t *slot(const char *k, char t) { item_t *it = get(k, t); if (it) return it; assert(N < 64); it = &T[N++]; memset(it, 0, sizeof *it); snprintf(it->ns, sizeof it->ns, "%s", g_ns); snprintf(it->key, sizeof it->key, "%s", k); it->type = t; return it; }
esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h) {
    int have = 0; for (int i = 0; i < N; i++) if (!strcmp(T[i].ns, ns)) have = 1;
    if (!have && m == NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
    g_ns = strcmp(ns, "petcfg") == 0 ? "petcfg" : "other"; *h = 1; return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *out, size_t *len) { (void)h; item_t *it = get(k, 's'); if (!it) return ESP_ERR_NVS_NOT_FOUND; size_t n = strlen(it->s) + 1; if (n > *len) return ESP_ERR_NVS_INVALID_LENGTH; memcpy(out, it->s, n); *len = n; return ESP_OK; }
esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v) { (void)h; item_t *old = get(k, 's'); if (old && !strcmp(old->s, v)) return ESP_OK; item_t *it = slot(k, 's'); free(it->s); it->s = strdup(v); WRITES++; return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *out, size_t *len) { (void)h; item_t *it = get(k, 'b'); if (!it) return ESP_ERR_NVS_NOT_FOUND; if (it->blen > *len) return ESP_ERR_NVS_INVALID_LENGTH; memcpy(out, it->b, it->blen); *len = it->blen; return ESP_OK; }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t n) { (void)h; item_t *old = get(k, 'b'); if (old && old->blen == n && !memcmp(old->b, v, n)) return ESP_OK; item_t *it = slot(k, 'b'); free(it->b); it->b = malloc(n); memcpy(it->b, v, n); it->blen = n; WRITES++; return ESP_OK; }
esp_err_t nvs_get_i64(nvs_handle_t h, const char *k, int64_t *v) { (void)h; item_t *it = get(k, 'i'); if (!it) return ESP_ERR_NVS_NOT_FOUND; *v = it->i; return ESP_OK; }
esp_err_t nvs_get_i32(nvs_handle_t h, const char *k, int32_t *v) { (void)h; item_t *it = get(k, 'i'); if (!it) return ESP_ERR_NVS_NOT_FOUND; *v = (int32_t)it->i; return ESP_OK; }
esp_err_t nvs_set_i64(nvs_handle_t h, const char *k, int64_t v) { (void)h; slot(k, 'i')->i = v; return ESP_OK; }
esp_err_t nvs_set_i32(nvs_handle_t h, const char *k, int32_t v) { (void)h; slot(k, 'i')->i = v; return ESP_OK; }
esp_err_t nvs_get_u8(nvs_handle_t h, const char *k, uint8_t *v) { (void)h; item_t *it = get(k, 'i'); if (!it) return ESP_ERR_NVS_NOT_FOUND; *v = (uint8_t)it->i; return ESP_OK; }
esp_err_t nvs_set_u8(nvs_handle_t h, const char *k, uint8_t v) { (void)h; slot(k, 'i')->i = v; return ESP_OK; }
esp_err_t nvs_erase_all(nvs_handle_t h) { (void)h; int k = 0; for (int i = 0; i < N; i++) { if (!strcmp(T[i].ns, g_ns)) { free(T[i].s); free(T[i].b); } else T[k++] = T[i]; } N = k; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }

// ESP-IDF nvs_page.cpp 的占项规则:字符串 1+ceil((len+1)/32);blob 1+ceil(n/32)+1(index);整数 1;命名空间 1。
static int entries_of(const item_t *it) {
    if (it->type == 's') return 1 + (int)((strlen(it->s) + 1 + 31) / 32);
    if (it->type == 'b') return 1 + (int)((it->blen + 31) / 32) + 1;
    return 1;
}
static int petcfg_entries(void) { int n = 1; for (int i = 0; i < N; i++) if (!strcmp(T[i].ns, "petcfg")) n += entries_of(&T[i]); return n; }
static const char *S(const char *k) { g_ns = "petcfg"; item_t *it = get(k, 's'); return it ? it->s : NULL; }

static void put_s(const char *k, const char *v) { g_ns = "petcfg"; item_t *it = slot(k, 's'); free(it->s); it->s = strdup(v); }
static void put_i(const char *k, int64_t v) { g_ns = "petcfg"; slot(k, 'i')->i = v; }
static void reset(void) { for (int i = 0; i < N; i++) { free(T[i].s); free(T[i].b); } N = 0; WRITES = 0; }

// 造一张 v4.7.2 写出的 petcfg(没有 wifis 键)
static void v472_image(void) {
    reset();
    char tok[1101], ref[1101]; memset(tok, 'A', 1100); tok[1100] = 0; memset(ref, 'B', 1100); ref[1100] = 0;
    put_s("ssid", "HomeNet"); put_s("pass", "secretpass1"); put_s("uid", "1234567890"); put_s("token", tok); put_s("refresh", ref);
    put_s("sn", "0000000000TEST1"); put_s("host", "cn.mqtt.bambulab.com"); put_s("dname", "Printer"); put_s("lan_ip", ""); put_s("lan_code", "");
    put_s("sns", "0000000000TEST1"); put_s("dnames", "Printer"); put_s("dprods", "H2C"); put_i("texp", 1792000000); put_i("remday", 20376);
}
static app_config_t C;
static wifi_slots_t W;
static void boot(void) { app_config_load(&C); }   // 开机:先读旧键,再 wifi_load 对账

int main(void) {
    // ===== A. v4.7.2 → v4.8 第一次开机:迁移 =====
    v472_image();
    int before = petcfg_entries();
    item_t snap[64]; int nsnap = N; memcpy(snap, T, sizeof T);                        // 浅拷贝用于比较旧键(字符串指针不会被改动前释放)
    boot();
    assert(app_config_cloud_ready(&C));
    assert(app_config_wifi_load(&W, &C) == ESP_OK);
    assert(W.n == 1 && !strcmp(W.s[0].ssid, "HomeNet") && !strcmp(W.s[0].pass, "secretpass1"));
    g_ns = "petcfg"; assert(get("wifis", 'b') && get("wifis", 'b')->blen == 100);
    assert(WRITES == 1);                                                              // 只写了 blob 一次(+ 一个整数标记,不计入 WRITES);旧键值相同 → 不重写
    // app-only 升级:升级前所有旧键,升级后逐个一模一样
    for (int i = 0; i < nsnap; i++) { g_ns = "petcfg"; item_t *it = get(snap[i].key, snap[i].type); assert(it); if (it->type == 's') assert(!strcmp(it->s, snap[i].s)); else assert(it->i == snap[i].i); }
    printf("迁移:petcfg 占用 %d → %d 项(+%d = 1 组 blob 6 项 + wmig 标记 1 项)\n", before, petcfg_entries(), petcfg_entries() - before);
    assert(petcfg_entries() - before == 7);
    assert(app_config_wifi_migrated());                                               // 迁移置标记 → 配网页横幅

    // ===== B. 再开机:幂等,不写 =====
    WRITES = 0; boot(); assert(app_config_wifi_load(&W, &C) == ESP_OK && W.n == 1 && WRITES == 0);

    // ===== C. 配网页「添加」两组(load → add → save)=====
    assert(wifi_slots_add(&W, "Office", "off-pass") == WIFI_OK && wifi_slots_add(&W, "Cafe", "") == WIFI_OK);
    assert(app_config_wifi_save(&W) == ESP_OK);
    assert(!strcmp(S("ssid"), "HomeNet") && !strcmp(S("pass"), "secretpass1"));      // 第 0 组没变 → 旧键没变
    wifi_slots_t R; boot(); assert(app_config_wifi_load(&R, &C) == ESP_OK && R.n == 3 && !strcmp(R.s[1].ssid, "Office") && !strcmp(R.s[2].ssid, "Cafe") && R.s[2].pass[0] == 0);
    assert(petcfg_entries() - before == 13);                                          // 3 组 = 12 项 + wmig 1 项

    // ===== D. 删除第 0 组:旧键镜像跟着变成新的第 0 组 =====
    assert(wifi_slots_delete(&R, "HomeNet") == WIFI_OK && app_config_wifi_save(&R) == ESP_OK);
    assert(!strcmp(S("ssid"), "Office") && !strcmp(S("pass"), "off-pass"));
    boot(); assert(!strcmp(C.ssid, "Office") && app_config_cloud_ready(&C));          // 降级回 v4.7.x 会连 Office
    assert(app_config_wifi_load(&W, &C) == ESP_OK && W.n == 2 && !strcmp(W.s[0].ssid, "Office"));   // 对账无变化
    WRITES = 0; assert(app_config_wifi_load(&W, &C) == ESP_OK && WRITES == 0);

    // ===== E. 降级到 v4.7.x 后在旧固件里重新配网(只改旧键),再升级 =====
    put_s("ssid", "NewPlace"); put_s("pass", "np-pass");                              // 旧固件 h_bind 写的
    boot(); assert(app_config_wifi_load(&W, &C) == ESP_OK);
    assert(W.n == 3 && !strcmp(W.s[0].ssid, "NewPlace") && !strcmp(W.s[1].ssid, "Office") && !strcmp(W.s[2].ssid, "Cafe"));
    // 旧固件配网换成列表里已有的 Cafe(开放网络)
    put_s("ssid", "Cafe"); put_s("pass", "");
    boot(); assert(app_config_wifi_load(&W, &C) == ESP_OK && W.n == 3 && !strcmp(W.s[0].ssid, "Cafe") && !strcmp(W.s[1].ssid, "NewPlace"));

    // ===== F. 满 5 组后新绑定的挤掉最久没用的 =====
    assert(wifi_slots_add(&W, "W4", "p4") == WIFI_OK && wifi_slots_add(&W, "W5", "p5") == WIFI_OK && app_config_wifi_save(&W) == ESP_OK && W.n == 5);
    assert(wifi_slots_add(&W, "W6", "p6") == WIFI_E_FULL);
    assert(petcfg_entries() - before == 19);                                          // 5 组 = 18 项 + wmig 1 项
    put_s("ssid", "Brand"); put_s("pass", "bp");
    boot(); assert(app_config_wifi_load(&W, &C) == ESP_OK && W.n == 5 && !strcmp(W.s[0].ssid, "Brand") && wifi_slots_find(&W, "W5") == -1);
    printf("5 组:petcfg 占用 %d 项(相对 v4.7.2 的 %d 项 +%d)\n", petcfg_entries(), before, petcfg_entries() - before);

    // ===== G. 坏 blob(被截断):按旧键重建,不崩 =====
    g_ns = "petcfg"; { item_t *b = get("wifis", 'b'); b->blen -= 7; }
    boot(); assert(app_config_wifi_load(&W, &C) == ESP_OK && W.n == 1 && !strcmp(W.s[0].ssid, "Brand"));
    g_ns = "petcfg"; { item_t *b = get("wifis", 'b'); b->b[0] = 9; }                // 版本号不认识
    boot(); assert(app_config_wifi_load(&W, &C) == ESP_OK && W.n == 1);

    // ===== H. 恢复出厂:app_config_erase 清 petcfg,新键一起没;之后未绑定、不写任何东西 =====
    assert(app_config_erase() == ESP_OK);
    g_ns = "petcfg"; assert(!get("wifis", 'b') && !get("ssid", 's'));
    WRITES = 0; boot(); assert(!app_config_cloud_ready(&C) && app_config_wifi_load(&W, &C) == ESP_OK && W.n == 0 && WRITES == 0);

    // ===== I. 全新出厂(没有 petcfg 命名空间)=====
    reset(); boot(); WRITES = 0; assert(app_config_wifi_load(&W, &C) == ESP_OK && W.n == 0 && WRITES == 0);

    // ===== J. 最坏情况的 petcfg 占用:SSID 32 + 口令 64 + 5 组 =====
    v472_image();
    char s32[33], p64[65]; memset(s32, 's', 32); s32[32] = 0; memset(p64, 'p', 64); p64[64] = 0;
    put_s("ssid", s32); put_s("pass", p64);
    boot(); assert(app_config_wifi_load(&W, &C) == ESP_OK && W.n == 1);
    for (int i = 0; i < 4; i++) { char a[8]; snprintf(a, sizeof a, "x%d", i); wifi_slots_add(&W, a, p64); }
    app_config_wifi_save(&W);
    printf("最坏情况(满 5 组、SSID/口令都最长、两个 token 各 1100 字符):petcfg %d 项;NVS 分区 756 项,可用 630 项\n", petcfg_entries());
    assert(petcfg_entries() < 630 / 2);

    // ===== K. migrated 标记的生命周期 =====
    v472_image(); boot();
    assert(!app_config_wifi_migrated());                                              // 还没 load:没有标记
    assert(app_config_wifi_load(&W, &C) == ESP_OK && app_config_wifi_migrated());     // 升级后首次开机:置位
    boot(); assert(app_config_wifi_load(&W, &C) == ESP_OK && app_config_wifi_migrated());   // 再开机、没人确认:仍置位(横幅一直在,直到用户操作)
    assert(app_config_wifi_clear_migrated() == ESP_OK && !app_config_wifi_migrated());      // 配网页成功增/删/完成 → 清零
    boot(); assert(app_config_wifi_load(&W, &C) == ESP_OK && !app_config_wifi_migrated());  // 清零后不再复现
    int e0 = petcfg_entries(); assert(app_config_wifi_clear_migrated() == ESP_OK && petcfg_entries() == e0);   // 重复清零无副作用
    // 坏 blob 重建不算"迁移"
    v472_image(); boot(); app_config_wifi_load(&W, &C); app_config_wifi_clear_migrated();
    g_ns = "petcfg"; { item_t *b = get("wifis", 'b'); b->b[0] = 9; }
    boot(); assert(app_config_wifi_load(&W, &C) == ESP_OK && W.n == 1 && !app_config_wifi_migrated());
    // 恢复出厂:标记随命名空间一起清掉
    v472_image(); boot(); app_config_wifi_load(&W, &C); assert(app_config_wifi_migrated());
    assert(app_config_erase() == ESP_OK && !app_config_wifi_migrated());

    // ===== L. 全新用户首次绑定(h_bind 写旧键后调 app_config_wifi_bound):不是迁移,不显示横幅 =====
    reset(); boot(); assert(!app_config_cloud_ready(&C));
    put_s("ssid", "Home-5G"); put_s("pass", "newuser-pass"); put_s("uid", "42"); put_s("token", "t"); put_s("sn", "SN1");
    boot(); assert(app_config_wifi_bound(&C) == ESP_OK && !app_config_wifi_migrated());
    g_ns = "petcfg"; assert(get("wifis", 'b') && get("wifis", 'b')->blen == 100);     // 绑定后 blob 立刻存在
    boot(); assert(app_config_wifi_load(&W, &C) == ESP_OK && W.n == 1 && !app_config_wifi_migrated());   // 下次开机也不会当成迁移
    // 已有列表的设备「换打印机,重新走完整设置」时在第 1 步选了列表里另一个 Wi-Fi:提到第 0 组,标记仍为 0
    assert(wifi_slots_add(&W, "Office", "off-pass1") == WIFI_OK && app_config_wifi_save(&W) == ESP_OK);
    put_s("ssid", "Office"); put_s("pass", "off-pass1");
    boot(); assert(app_config_wifi_bound(&C) == ESP_OK && !app_config_wifi_migrated());
    boot(); assert(app_config_wifi_load(&W, &C) == ESP_OK && W.n == 2 && !strcmp(W.s[0].ssid, "Office") && !strcmp(W.s[1].ssid, "Home-5G"));

    // ---- v4.8 休眠设置:petcfg/sleep_min、petcfg/sleep_face(u8)load/save/默认/非法值 ----
    reset(); boot(); assert(C.sleep_min == 2 && C.sleep_face == 0);                       // 全新/擦除:默认 2 分钟 + 全黑
    v472_image(); boot(); assert(C.sleep_min == 2 && C.sleep_face == 0);                  // v4.7.2 升级上来没有这两个键:默认
    int s0 = petcfg_entries();
    assert(app_config_save_sleep_min(5) == ESP_OK && app_config_save_sleep_face(SLEEP_FACE_PET) == ESP_OK);
    assert(petcfg_entries() - s0 == 2);                                                   // 各占 1 个 NVS 条目
    boot(); assert(C.sleep_min == 5 && C.sleep_face == 2);
    assert(app_config_save_sleep_min(0) == ESP_OK && app_config_save_sleep_face(SLEEP_FACE_CLOCK) == ESP_OK);
    boot(); assert(C.sleep_min == 0 && C.sleep_face == 1);                                // 0 = 关,合法
    put_i("sleep_min", 7); put_i("sleep_face", 9); boot(); assert(C.sleep_min == 2 && C.sleep_face == 0);   // 非法值按默认
    put_i("sleep_min", 255); put_i("sleep_face", 3); boot(); assert(C.sleep_min == 2 && C.sleep_face == 0);
    assert(app_config_save_sleep_min(3) == ESP_OK && app_config_save_sleep_face(200) == ESP_OK);   // 写入前也校验
    g_ns = "petcfg"; assert(get("sleep_min", 'i')->i == 2 && get("sleep_face", 'i')->i == 0);
    assert(app_config_erase() == ESP_OK); boot(); assert(C.sleep_min == 2 && C.sleep_face == 0);   // 恢复出厂回默认
    // 设置页中键循环
    assert(sleep_min_next(0) == 2 && sleep_min_next(2) == 5 && sleep_min_next(5) == 0 && sleep_min_next(9) == 5);
    assert(sleep_face_next(0) == 1 && sleep_face_next(1) == 2 && sleep_face_next(2) == 0 && sleep_face_next(7) == 1);
    printf("sleep_min/sleep_face load/save/default/invalid: PASS\n");

    reset();
    printf("test_wifi_nvs_v48 OK\n");
    return 0;
}
