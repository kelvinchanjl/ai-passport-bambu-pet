#!/usr/bin/env python3
"""v4.5.2: the hotspot name on screen, the WIFI: payload in the QR code, and the SSID the firmware sets must match exactly.
Builds tests/test_ap_name_v452.c against main/pet_logic.c (the helpers net.c and pet_ui.c both call) and LVGL's bundled
qrcodegen.c (same call as lv_qrcode_update: ECC M, min version, auto mask), renders the module matrix at the firmware's
3 px/module on white, decodes it with OpenCV, and also checks net.c / pet_ui.c actually route through those helpers."""
import os, re, subprocess, sys, tempfile
R = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
LV = os.path.join(R, "managed_components", "lvgl__lvgl", "src", "libs", "qrcode")

def main(out_png=None):
    src = lambda p: open(os.path.join(R, "main", p), encoding="utf-8").read()
    net, ui = src("net.c"), src("pet_ui.c")
    assert "PrintPal" not in re.sub(r"//.*", "", net) and "PrintPal" not in ui, "old SSID prefix still in code"
    assert re.search(r"pet_ap_name\(mac, name_out, len\)", net) and re.search(r"strlcpy\(\(char \*\)ap\.ap\.ssid, name_out", net)
    assert "pet_ap_qr(arg, q, sizeof q)" in ui and "lv_qrcode_update(s_qr, q, strlen(q))" in ui and "pet_ap_step(arg, st, sizeof st)" in ui
    with tempfile.TemporaryDirectory() as t:
        for f in ("qrcodegen.c", "qrcodegen.h"):
            s = open(os.path.join(LV, f), encoding="utf-8").read()
            s = s.replace('#include "../../misc/lv_assert.h"', '#include <assert.h>\n#define LV_ASSERT(x) assert(x)')
            s = s.replace('#include "../../../lvgl.h"', '#define LV_USE_QRCODE 1\n#define LV_STDBOOL_INCLUDE <stdbool.h>\n#define LV_STDDEF_INCLUDE <stddef.h>\n#define LV_STDINT_INCLUDE <stdint.h>')
            open(os.path.join(t, f), "w").write(s)
        exe = os.path.join(t, "ap")
        subprocess.check_call([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-I" + t, "-I" + os.path.join(R, "main"),
                               os.path.join(R, "tests", "test_ap_name_v452.c"), os.path.join(R, "main", "pet_logic.c"),
                               os.path.join(t, "qrcodegen.c"), "-o", exe])
        txt = subprocess.check_output([exe], text=True)
    kv = dict(l.split("=", 1) for l in txt.splitlines() if "=" in l)
    rows = [l for l in txt.splitlines() if l and set(l) <= {"#", "."}]
    ssid = kv["SSID"]
    assert re.fullmatch(r"Passport-[0-9A-F]{4}", ssid), ssid
    assert ssid == "Passport-12AB"                          # sample SoftAP MAC ...:12:ab
    assert kv["STEP"] == "连上热点 " + ssid
    assert kv["QR"] == "WIFI:T:nopass;S:%s;;" % ssid
    from PIL import Image
    import cv2, numpy as np
    k, pad = 3, 8                                          # firmware: 3 px/module, 8 px white margin (103 px box)
    n = len(rows); im = Image.new("L", (n * k + 2 * pad,) * 2, 255); px = im.load()
    for y, r in enumerate(rows):
        for x, c in enumerate(r):
            if c == "#":
                for dy in range(k):
                    for dx in range(k): px[pad + x * k + dx, pad + y * k + dy] = 0
    big = im.resize((im.width * 4, im.height * 4), Image.NEAREST)
    if out_png: big.save(out_png)
    dec, _, _ = cv2.QRCodeDetector().detectAndDecode(np.array(big))
    assert dec == kv["QR"], (dec, kv["QR"])
    m = re.fullmatch(r"WIFI:T:nopass;S:(.*);;", dec); assert m and m.group(1) == ssid
    print("ap name v4.5.2: SSID=%s | screen=「%s」 | QR decoded=%s (v%s, %sx%s, %d px) | all three match: PASS"
          % (ssid, kv["STEP"], dec, kv["VERSION"], kv["SIZE"], kv["SIZE"], n * k))

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else None)
