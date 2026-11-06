"""v4.8 hotfix contract: B 首帧不得在 pet_app 上同步 lv_refr_now;B 机器名不用 LONG_DOT。
主机没有 SDL/LVGL 显示,不能真的 refr 一帧;三种画面的标签文本由 test_sleep_v48 的
pet_sleep_compose_view 覆盖。本文件检查源码里的刷屏状态机和栈大小。
Run: python3 tests/test_sleep_ui_contract_v48.py"""
import os, re, sys
R = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
main = open(os.path.join(R, "main/main.c")).read()
ui = open(os.path.join(R, "main/pet_ui.c")).read()
bad = []

def fn(src, name, nxt):
    pat = r"static void %s\b.*?\n(?=static void %s\b)" % (name, nxt)
    m = re.search(pat, src, re.S)
    return m.group(0) if m else ""

enter = fn(main, "sleep_enter", "sleep_wake")
wake = fn(main, "sleep_wake", "sleep_tick")
if not enter:
    bad.append("sleep_enter not found")
if not wake:
    bad.append("sleep_wake not found")

# A 分支在 panel_sleep 之后 return;B/C 是后半段
bc = enter.split("return;")[-1] if enter else ""
if "pet_ui_sleep_enter" not in bc:
    bad.append("B/C path missing pet_ui_sleep_enter")
if "pet_ui_refresh_now" in bc or "lv_refr_now" in bc:
    bad.append("B/C sleep_enter still sync-refreshes on pet_app")
if "pet_ui_render_pause(false)" not in bc:
    bad.append("B/C path does not resume render_pause before loading the sleep screen")
if "sleep_yield_frame" not in bc:
    bad.append("B/C path does not yield the first frame to taskLVGL")
if "pet_ui_refresh_now" in wake or "lv_refr_now" in wake:
    bad.append("sleep_wake still sync-refreshes on pet_app")

m = re.search(r'xTaskCreate\(app_task,\s*"pet_app",\s*(\d+)', main)
if not m:
    bad.append("pet_app xTaskCreate not found")
else:
    stack = int(m.group(1))
    if stack != 5120:
        bad.append("pet_app stack %d != 5120" % stack)

idx = ui.find("s_slp_name = slp_label")
if idx < 0:
    bad.append("s_slp_name create missing")
else:
    chunk = ui[idx:idx + 900]
    if "LV_LABEL_LONG_DOT" in chunk:
        bad.append("s_slp_name still uses LONG_DOT")
    if "LV_LABEL_LONG_CLIP" not in chunk:
        bad.append("s_slp_name does not use LONG_CLIP")
    if "lv_obj_set_height(s_slp_name" not in chunk:
        bad.append("s_slp_name has no fixed line height")
    if "LV_OBJ_FLAG_HIDDEN" not in chunk:
        bad.append("s_slp_name not hidden before first layout")

c_start = ui.find("if (face == SLEEP_FACE_PET)")
c_end = ui.find("s_slp_time = slp_label(s_slp_wrap, &font_pet_36")
if c_start < 0 or c_end < 0 or c_end <= c_start:
    bad.append("could not isolate C vs B path in pet_ui_sleep_enter")
else:
    c = ui[c_start:c_end]
    if "LV_LABEL_LONG_DOT" in c:
        bad.append("C path uses LONG_DOT")
    if "s_slp_name" in c:
        bad.append("C path unexpectedly creates s_slp_name")

print("sleep UI contract:", "PASS" if not bad else "FAIL " + "; ".join(bad))
sys.exit(1 if bad else 0)
