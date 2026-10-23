"""v4.4.1 static contract for the real-device CPU hog (not a scheduler simulation).
The real fix is in pet_ui.c (no unconditional move_foreground every 50 ms); this also checks the
cloud task can preempt the LVGL task. Run: python3 tests/test_cpu_contract_v441.py"""
import os, re, sys
R = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
ui = open(os.path.join(R, "main/pet_ui.c")).read()
cloud = open(os.path.join(R, "main/bambu_cloud.c")).read()
port = open(os.path.join(R, "managed_components/espressif__esp_lvgl_port/include/esp_lvgl_port.h")).read()
bad = []
hv = ui[ui.index("static void update_header_visibility(void) {"):]; hv = hv[:hv.index("\n}\n")]
if "if (!hdr_on_top())" not in hv: bad.append("update_header_visibility moves objects to the foreground unconditionally")
dp = ui[ui.index("static void dots_place(void) {"):]; dp = dp[:dp.index("\n}\n")]
if "last_y" not in dp: bad.append("dots_place re-applies styles every call")
upd = ui[ui.index("void pet_ui_update("):]; upd = upd[:upd.index("\n}\n")]
if re.search(r"\n    update_detail\(st, s, link\);", upd): bad.append("update_detail runs while the detail page is hidden")
if re.search(r"\blv_label_set_text(_fmt)?\(", upd) or "lv_obj_set_style_text_font(" in upd: bad.append("raw LVGL setters in pet_ui_update (use change-only UI_* wrappers)")
lv = int(re.search(r"\.task_priority = (\d+)", port).group(1))
tok = int(re.search(r"#define BAMBU_TOK_PRIO (\d+)", cloud).group(1))
if not tok > lv: bad.append("bambu_tok priority %d not above LVGL %d" % (tok, lv))
print("cpu contract: LVGL prio %d, bambu_tok prio %d:" % (lv, tok), "PASS" if not bad else "FAIL " + "; ".join(bad))
sys.exit(1 if bad else 0)
