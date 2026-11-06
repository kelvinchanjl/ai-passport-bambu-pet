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

# 熄屏时不得改首页对象(真机 B 段 job 结束后曾 Load access fault)
if "if (!s_asleep && bsp_lvgl_lock(100))" not in main or "pet_ui_update(states[cur]" not in main:
    bad.append("pet_ui_update is not skipped while asleep")
if "pet_sleep_content_changed" not in main:
    bad.append("B/C sleep tick does not refresh on job/name/pct change")

# sleep_exit 先摘指针并 lv_screen_load 首页,再恢复 top;exit 内禁止任何 delete
# (含旧 s_slp_gone)。B 醒曾崩在 lv_obj_remove_flag(lv_layer_top(), HIDDEN):
# layer_top parent 为 NULL,LVGL 9 mark_layout_as_dirty 不判空(MTVAL=0x2a)。
ex = ui.find("void pet_ui_sleep_exit")
reap = ui.find("void pet_ui_sleep_reap")
if ex < 0:
    bad.append("pet_ui_sleep_exit missing")
else:
    end = reap if reap > ex else ex + 1600
    chunk = ui[ex:end]
    if "lv_obj_t *gone = s_slp_scr" not in chunk:
        bad.append("sleep_exit does not snapshot s_slp_scr before nulling")
    if "s_slp_name =" not in chunk:
        bad.append("sleep_exit does not null s_slp_* pointers")
    if "lv_obj_delete(" in chunk:
        bad.append("sleep_exit still calls lv_obj_delete")
    if "s_slp_gone" not in chunk:
        bad.append("sleep_exit does not defer delete via s_slp_gone")
    load = chunk.find("lv_screen_load")
    unhide = chunk.find("slp_unhide_top")
    if load < 0:
        bad.append("sleep_exit does not lv_screen_load the home screen")
    if unhide < 0:
        bad.append("sleep_exit does not unhide top via slp_unhide_top")
    elif load >= 0 and not (load < unhide):
        bad.append("sleep_exit unhides top before lv_screen_load")

h = ui.find("static void slp_unhide_top")
if h < 0:
    bad.append("slp_unhide_top helper missing")
else:
    hchunk = ui[h:ex] if ex > h else ui[h:h + 1400]
    if "lv_obj_get_parent" not in hchunk:
        bad.append("slp_unhide_top does not check parent before remove_flag")
    if "lv_obj_is_valid" not in hchunk:
        bad.append("slp_unhide_top does not check object validity")
    if "LV_OBJ_FLAG_HIDDEN" not in hchunk:
        bad.append("slp_unhide_top does not handle HIDDEN")
if reap < 0:
    bad.append("pet_ui_sleep_reap missing")
else:
    rchunk = ui[reap:reap + 600]
    if "lv_obj_delete(gone)" not in rchunk:
        bad.append("sleep_reap does not delete the old sleep screen")

# B/C 唤醒:exit → yield 一帧 → reap(A 路径里也有一次 yield,只看 exit 到 reap 之间)
if "pet_ui_sleep_reap" not in wake:
    bad.append("sleep_wake does not reap the old B/C screen")
else:
    we, wr = wake.find("pet_ui_sleep_exit"), wake.find("pet_ui_sleep_reap")
    wy = wake.rfind("sleep_yield_frame", 0, wr) if wr >= 0 else -1
    if not (0 <= we < wy < wr):
        bad.append("sleep_wake does not yield a frame between sleep_exit and sleep_reap")
if "pet_ui_sleep_reap" not in enter:
    bad.append("sleep_enter does not reap a leftover sleep screen after the first frame")
else:
    er = enter.find("pet_ui_sleep_reap")
    ey = enter.rfind("sleep_yield_frame", 0, er)
    if not (0 <= ey < er):
        bad.append("sleep_enter does not reap after yielding the first B/C frame")

# C 刷新不得对空 hair 指针换色
upd = ui.find("void pet_ui_sleep_update")
if upd < 0:
    bad.append("pet_ui_sleep_update missing")
else:
    uchunk = ui[upd:upd + 900]
    if "if (s_slp_hair)" not in uchunk or "lv_obj_set_style_image_recolor(s_slp_hair" not in uchunk:
        bad.append("C sleep_update does not null-check s_slp_hair before recolor")

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
