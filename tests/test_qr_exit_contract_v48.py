"""v4.8 扫码页长按返回:UI 定稿 + 退出逻辑契约。
文字「长按中键 返回」(半角空格),字库不补「退」;仅设置→重新配网显示。
Run: python3 tests/test_qr_exit_contract_v48.py
"""
import os, re, sys
R = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
main = open(os.path.join(R, "main/main.c"), encoding="utf-8").read()
ui = open(os.path.join(R, "main/pet_ui.c"), encoding="utf-8").read()
logic = open(os.path.join(R, "main/pet_logic.h"), encoding="utf-8").read()
portal = open(os.path.join(R, "main/setup_portal.c"), encoding="utf-8").read()
chars = open(os.path.join(R, "main/assets/font_chars.txt"), encoding="utf-8").read()
bad = []

if 'PET_QR_EXIT_HINT "长按中键 返回"' not in logic:
    bad.append("PET_QR_EXIT_HINT is not exactly 长按中键 返回 (half-width space)")
if "长按中键 退出" in logic or "长按中键 退出" in ui or "长按中键 退出" in main:
    bad.append("source still uses 退出 instead of 返回")
if "退" in chars.splitlines()[0] if chars.splitlines() else True:
    # 字库不改:font_chars 原先就没有「退」,也不该新加
    line = chars.splitlines()[0] if chars.splitlines() else ""
    if "退" in line:
        bad.append("font_chars.txt gained 退 (font must not change)")

if "s_qr_hint = su_lbl(s_qr_box, &font_pet_12, SU_GR, 262)" not in ui:
    bad.append("s_qr_hint not created with su_lbl(..., font_pet_12, SU_GR, 262)")
if "lv_label_set_text(s_qr_hint, PET_QR_EXIT_HINT)" not in ui:
    bad.append("s_qr_hint text not set to PET_QR_EXIT_HINT")
# UI:hint 进「重新配网」才建、退出即删,不要在 build_setup 里常驻
bs = ui.find("static void build_setup")
be = ui.find("static void destroy_qr_exit_hint")
if bs < 0 or be < 0 or be <= bs:
    bad.append("build_setup / destroy_qr_exit_hint missing")
elif "s_qr_hint = su_lbl" in ui[bs:be]:
    bad.append("s_qr_hint is still created inside build_setup (must not be resident)")
if "void pet_ui_qr_exit_hint(bool show)" not in ui:
    bad.append("pet_ui_qr_exit_hint missing")
if "create_qr_exit_hint()" not in ui:
    bad.append("pet_ui_qr_exit_hint does not create hint on show")
if "destroy_qr_exit_hint()" not in ui:
    bad.append("hint is not deleted on hide/teardown")
if "set_hidden(s_qr_hint, true)" not in ui:
    bad.append("setup_apply does not hide hint by default on QR page")
if "teardown_setup()" not in ui or "if (sc == UI_PET) { teardown_setup(); return; }" not in ui:
    bad.append("setup pages/hint are not torn down when returning to UI_PET")

if "pet_portal_exit_plan(s_cloud_ok, reauth)" not in main:
    bad.append("start_portal does not use pet_portal_exit_plan(s_cloud_ok, reauth)")
if "pet_ui_qr_exit_hint(s_portal_cancellable)" not in main:
    bad.append("start_portal does not show hint only when cancellable")
if "UI_SETUP_QR && s_portal_cancellable" not in main or "stop_portal_to_prev" not in main:
    bad.append("long-press on QR does not call stop_portal_to_prev when cancellable")
if "setup_portal_stop()" not in main:
    bad.append("stop_portal_to_prev does not call setup_portal_stop")
if "s_set_sel = SET_REPAIR" not in main:
    bad.append("exit does not restore settings at 重新配网")
if "void setup_portal_stop(void)" not in portal or "net_ap_stop()" not in portal:
    bad.append("setup_portal_stop missing or does not stop AP")
if "nvs kept" not in portal:
    bad.append("setup_portal_stop log should note nvs kept")

print("qr exit contract:", "PASS" if not bad else "FAIL " + "; ".join(bad))
sys.exit(1 if bad else 0)
