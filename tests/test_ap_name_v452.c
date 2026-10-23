// tests/test_ap_name_v452.c — prints the SSID / on-screen step / QR payload produced by the firmware helpers,
// and the QR module matrix encoded by LVGL's own qrcodegen with the exact lv_qrcode_update() parameters.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "pet_logic.h"
#include "qrcodegen.h"
int main(int argc, char **argv) {
    unsigned m[6] = { 0x02, 0x00, 0x00, 0x00, 0x12, 0xAB };   // 示例 SoftAP MAC(本地管理地址,非真实设备)
    if (argc > 1) sscanf(argv[1], "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]);
    uint8_t mac[6]; for (int i = 0; i < 6; i++) mac[i] = (uint8_t)m[i];
    char ssid[33], q[96], step[64];
    pet_ap_name(mac, ssid, sizeof ssid);     // net.c: wifi_config_t.ap.ssid = 这个
    pet_ap_qr(ssid, q, sizeof q);            // pet_ui.c: lv_qrcode_update(q)
    pet_ap_step(ssid, step, sizeof step);    // pet_ui.c: 第 2 步文字
    assert(strlen(ssid) <= 32);
    uint8_t qr[qrcodegen_BUFFER_LEN_MAX], tmp[qrcodegen_BUFFER_LEN_MAX];
    size_t n = strlen(q); memcpy(tmp, q, n);
    int ver = qrcodegen_getMinFitVersion(qrcodegen_Ecc_MEDIUM, n);
    assert(qrcodegen_encodeBinary(tmp, n, qr, qrcodegen_Ecc_MEDIUM, ver, ver, qrcodegen_Mask_AUTO, true));
    int sz = qrcodegen_getSize(qr);
    printf("SSID=%s\nSTEP=%s\nQR=%s\nVERSION=%d\nSIZE=%d\n", ssid, step, q, ver, sz);
    for (int y = 0; y < sz; y++) { for (int x = 0; x < sz; x++) putchar(qrcodegen_getModule(qr, x, y) ? '#' : '.'); putchar('\n'); }
    return 0;
}
