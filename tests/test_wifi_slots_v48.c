// v4.8 多组 Wi-Fi 的存取逻辑:上限 5 组、增删改、绑定/对账(迁移)、blob 序列化、NVS 占用。纯 C,主机可跑。
#include "wifi_slots.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static wifi_slots_t S;
static void fill(int n) { wifi_slots_init(&S); char a[24], b[24]; for (int i = 0; i < n; i++) { snprintf(a, sizeof a, "net%d", i); snprintf(b, sizeof b, "pw%d", i); assert(wifi_slots_add(&S, a, b) == WIFI_OK); } }

// 独立重写一遍 ESP-IDF nvs_page.cpp writeItem 的项数规则(变长项 = 1 个头 + 数据按 32 B 向上取整),用来核对 wifi_nvs_*。
static int ref_var_entries(size_t bytes) { return 1 + (int)((bytes + 31) / 32); }

int main(void) {
    // ---- 常量 ----
    assert(WIFI_SLOTS_MAX == 5 && WIFI_SLOT_BYTES == 98 && WIFI_BLOB_MAX == 492);
    assert(sizeof(wifi_slot_t) == 98);

    // ---- 校验 ----
    char s32[33], s33[34], p64[65], p65[66];
    memset(s32, 'a', 32); s32[32] = 0; memset(s33, 'a', 33); s33[33] = 0;
    memset(p64, 'p', 64); p64[64] = 0; memset(p65, 'p', 65); p65[65] = 0;
    assert(wifi_slots_valid("x", "") && wifi_slots_valid("x", NULL) && wifi_slots_valid(s32, p64));
    assert(!wifi_slots_valid("", "p") && !wifi_slots_valid(NULL, "p") && !wifi_slots_valid(s33, "p") && !wifi_slots_valid("x", p65));

    // ---- 增:到 5 组为止,第 6 个新 SSID 被拒 ----
    wifi_slots_init(&S);
    assert(S.n == 0);
    for (int i = 0; i < 5; i++) {
        char a[24]; snprintf(a, sizeof a, "n%d", i);
        assert(wifi_slots_add(&S, a, "pw") == WIFI_OK && S.n == i + 1);
    }
    assert(wifi_slots_add(&S, "n5", "pw") == WIFI_E_FULL && S.n == 5);
    assert(wifi_slots_find(&S, "n5") == -1);
    // 同名 = 更新口令,不占新槽,满了也允许
    assert(wifi_slots_add(&S, "n2", "newpw") == WIFI_OK && S.n == 5 && strcmp(S.s[2].pass, "newpw") == 0 && strcmp(S.s[2].ssid, "n2") == 0);
    assert(wifi_slots_add(&S, "n2", "") == WIFI_OK && S.s[2].pass[0] == 0);   // 改成开放网络
    assert(wifi_slots_add(&S, "", "x") == WIFI_E_INVALID && wifi_slots_add(&S, s33, "x") == WIFI_E_INVALID && wifi_slots_add(&S, "ok", p65) == WIFI_E_INVALID);
    assert(S.n == 5);
    // 最长 SSID/口令能完整存取
    fill(0); assert(wifi_slots_add(&S, s32, p64) == WIFI_OK && strcmp(S.s[0].ssid, s32) == 0 && strcmp(S.s[0].pass, p64) == 0);

    // ---- 删:中间/开头/末尾,后面的前移,尾部清零;最后一组不能删 ----
    fill(5);
    assert(wifi_slots_delete(&S, "net2") == WIFI_OK && S.n == 4);
    assert(!strcmp(S.s[0].ssid, "net0") && !strcmp(S.s[1].ssid, "net1") && !strcmp(S.s[2].ssid, "net3") && !strcmp(S.s[3].ssid, "net4"));
    assert(!strcmp(S.s[2].pass, "pw3"));
    assert(S.s[4].ssid[0] == 0 && S.s[4].pass[0] == 0);              // 尾部不留旧密码
    assert(wifi_slots_delete(&S, "net0") == WIFI_OK && S.n == 3 && !strcmp(S.s[0].ssid, "net1"));
    assert(wifi_slots_delete(&S, "net4") == WIFI_OK && S.n == 2);
    assert(wifi_slots_delete(&S, "nope") == WIFI_E_NOTFOUND && S.n == 2);
    assert(wifi_slots_delete(&S, "net1") == WIFI_OK && S.n == 1);
    assert(wifi_slots_delete(&S, "net3") == WIFI_E_LAST && S.n == 1);   // 至少留一组
    wifi_slots_init(&S);
    assert(wifi_slots_delete(&S, "x") == WIFI_E_NOTFOUND);
    // 删到 4 组之后又能再加一组
    fill(5); assert(wifi_slots_delete(&S, "net1") == WIFI_OK && wifi_slots_add(&S, "fresh", "p") == WIFI_OK && S.n == 5 && !strcmp(S.s[4].ssid, "fresh"));

    // ---- promote(绑定/对账):放到第 0 组 ----
    fill(3);
    assert(wifi_slots_promote(&S, "net2", "pw2") && S.n == 3 && !strcmp(S.s[0].ssid, "net2") && !strcmp(S.s[1].ssid, "net0") && !strcmp(S.s[2].ssid, "net1"));
    assert(!wifi_slots_promote(&S, "net2", "pw2"));                        // 已经是第 0 组且口令一样:无变化
    assert(wifi_slots_promote(&S, "net2", "other") && !strcmp(S.s[0].pass, "other") && S.n == 3);   // 改口令
    assert(wifi_slots_promote(&S, "brand", "bp") && S.n == 4 && !strcmp(S.s[0].ssid, "brand") && !strcmp(S.s[1].ssid, "net2"));
    fill(5);                                                               // 满 5 组:新来的挤掉最后一组
    assert(wifi_slots_promote(&S, "brand", "bp") && S.n == 5 && !strcmp(S.s[0].ssid, "brand") && !strcmp(S.s[1].ssid, "net0") && !strcmp(S.s[4].ssid, "net3"));
    assert(wifi_slots_find(&S, "net4") == -1);
    assert(!wifi_slots_promote(&S, "", "x") && !wifi_slots_promote(&S, s33, "x"));

    // ---- 迁移 / 对账:v4.7.x 的单组 ssid/pass → 第 0 组 ----
    wifi_slots_init(&S);
    assert(wifi_slots_reconcile(&S, "HomeNet", "secret123") && S.n == 1 && !strcmp(S.s[0].ssid, "HomeNet") && !strcmp(S.s[0].pass, "secret123"));
    assert(!wifi_slots_reconcile(&S, "HomeNet", "secret123"));            // 第二次开机:幂等,不再写 NVS
    assert(!wifi_slots_reconcile(&S, "", "x") && !wifi_slots_reconcile(&S, NULL, NULL) && S.n == 1);   // 没有旧 Wi-Fi:不动
    wifi_slots_add(&S, "Office", "o"); wifi_slots_add(&S, "Cafe", "");
    // 降级到 v4.7.x 后在旧固件里重新配网换了 Wi-Fi,再升级:旧键与第 0 组不同 → 提到第 0 组,其余保留
    assert(wifi_slots_reconcile(&S, "NewHome", "np") && S.n == 4 && !strcmp(S.s[0].ssid, "NewHome") && !strcmp(S.s[1].ssid, "HomeNet"));
    // 旧键等于列表里的非第 0 组(配网页刚绑定过那一组)→ 移到最前,不产生重复
    assert(wifi_slots_reconcile(&S, "Cafe", "") && !strcmp(S.s[0].ssid, "Cafe") && S.n == 4 && wifi_slots_find(&S, "Cafe") == 0);
    for (int i = 0; i < S.n; i++) for (int j = 0; j < i; j++) assert(strcmp(S.s[i].ssid, S.s[j].ssid));

    // ---- blob 序列化往返 ----
    uint8_t buf[WIFI_BLOB_MAX + 8], buf2[WIFI_BLOB_MAX];
    fill(5);
    size_t len = wifi_slots_pack(&S, buf, sizeof buf);
    assert(len == 492 && buf[0] == WIFI_BLOB_VER && buf[1] == 5);
    wifi_slots_t R;
    assert(wifi_slots_unpack(&R, buf, len) && memcmp(&R, &S, sizeof R) == 0);
    assert(wifi_slots_pack(&R, buf2, sizeof buf2) == len && memcmp(buf, buf2, len) == 0);   // 确定性:同一内容同一字节
    fill(1); len = wifi_slots_pack(&S, buf, sizeof buf); assert(len == 100 && wifi_slots_unpack(&R, buf, len) && R.n == 1);
    fill(0); len = wifi_slots_pack(&S, buf, sizeof buf); assert(len == 2 && wifi_slots_unpack(&R, buf, len) && R.n == 0);
    assert(wifi_slots_pack(&S, buf, 1) == 0);                              // 缓冲不够
    // 损坏的 blob 一律拒收且结果为空
    fill(3); len = wifi_slots_pack(&S, buf, sizeof buf);
    assert(!wifi_slots_unpack(&R, buf, len - 1) && R.n == 0);              // 长度不对
    assert(!wifi_slots_unpack(&R, buf, len + 1));
    assert(!wifi_slots_unpack(&R, buf, 1) && !wifi_slots_unpack(&R, NULL, 0));
    { uint8_t t[WIFI_BLOB_MAX]; memcpy(t, buf, len); t[0] = 2; assert(!wifi_slots_unpack(&R, t, len));           // 版本不认识
      memcpy(t, buf, len); t[1] = 6; assert(!wifi_slots_unpack(&R, t, len));                                     // 组数超上限
      memcpy(t, buf, len); t[2 + 32] = 'x'; assert(!wifi_slots_unpack(&R, t, len) && R.n == 0);                  // SSID 没有结尾 0
      memcpy(t, buf, len); t[2 + 33 + 64] = 'x'; assert(!wifi_slots_unpack(&R, t, len));                         // 口令没有结尾 0
      memcpy(t, buf, len); t[2] = 0; assert(!wifi_slots_unpack(&R, t, len));                                     // 空 SSID
      memcpy(t, buf, len); memcpy(t + 2 + 98, t + 2, 98); assert(!wifi_slots_unpack(&R, t, len)); }              // 重名

    // ---- NVS 占用:按 ESP-IDF 规则精确计算 ----
    // blob(n 组)= 1 头 + ceil((2+98n)/32) 数据 + 1 index
    static const int exp_bytes[] = { 0, 100, 198, 296, 394, 492 }, exp_ent[] = { 0, 6, 9, 12, 15, 18 };
    for (int n = 1; n <= 5; n++) {
        assert((int)wifi_nvs_blob_bytes(n) == exp_bytes[n]);
        assert(wifi_nvs_blob_entries(n) == exp_ent[n]);
        assert(wifi_nvs_blob_entries(n) == ref_var_entries(wifi_nvs_blob_bytes(n)) + 1);
    }
    // 旧单组键:ssid ≤32 → 1+ceil(33/32)=3 项;pass ≤64 → 1+ceil(65/32)=4 项;典型 10/12 字符 → 2/2 项
    assert(wifi_nvs_str_entries(32) == 3 && wifi_nvs_str_entries(64) == 4 && wifi_nvs_str_entries(10) == 2 && wifi_nvs_str_entries(12) == 2 && wifi_nvs_str_entries(0) == 2);
    // 新增总开销(5 组 blob 18 项;旧键 ssid/pass 本来就在,最坏 3+4=7 项,不新增)
    // 写入时旧 blob 在新 blob 提交前仍占着:峰值 2×18 = 36 项,24 KB NVS 分区共 6×126=756 项、可用 630 项(留 1 页 GC)
    assert(2 * wifi_nvs_blob_entries(5) == 36 && 36 < 630);
    // 单个 blob 必须放得进一页的一个 chunk(NVS 单页 126 项,blob 数据最多 125 项 = 4000 B)
    assert(wifi_nvs_blob_bytes(5) <= 4000);

    printf("test_wifi_slots_v48 OK (blob 5 slots = %zu B = %d NVS entries)\n", wifi_nvs_blob_bytes(5), wifi_nvs_blob_entries(5));
    return 0;
}
