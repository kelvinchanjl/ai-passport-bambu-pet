"""v4.8 heap tighten + debug-only status inject contract.
MQTT/JSON 缓冲下调;扫码 hint 进重新配网才建、退出删除;注入命令仅调试构建。
Run: python3 tests/test_heap_inject_contract_v48.py
"""
import os, re, sys
R = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
main = open(os.path.join(R, "main/main.c"), encoding="utf-8").read()
ui = open(os.path.join(R, "main/pet_ui.c"), encoding="utf-8").read()
cloud = open(os.path.join(R, "main/bambu_cloud.c"), encoding="utf-8").read()
kconfig = open(os.path.join(R, "main/Kconfig.projbuild"), encoding="utf-8").read()
defaults = open(os.path.join(R, "sdkconfig.defaults"), encoding="utf-8").read()
bad = []

# MQTT / JSON buffers
if "#define FILT_CAP (3 * 1024)" not in cloud:
    bad.append("FILT_CAP is not 3 KB")
if ".buffer = { .size = 2048, .out_size = 256 }" not in cloud:
    bad.append("MQTT buffer is not 2048/256")
if ".buffer = { .size = 4096, .out_size = 1024 }" in cloud:
    bad.append("old MQTT 4096/1024 buffer still present")

# Sleep panel already create-on-enter / destroy-on-exit
if "void pet_ui_sleep_enter" not in ui or "void pet_ui_sleep_exit" not in ui:
    bad.append("sleep panel enter/exit missing")
if "void pet_ui_sleep_reap" not in ui:
    bad.append("sleep reap (deferred delete) missing")
if "lv_obj_delete(gone)" not in ui:
    bad.append("old sleep screen is never deleted")
if "CONFIG_LV_MEM_SIZE_KILOBYTES=34" not in defaults:
    bad.append("LVGL pool is not 34 KB (32→34; system heap still has margin, QR page needs contiguous LVGL room)")
if "CONFIG_LV_MEM_SIZE_KILOBYTES=32" in defaults:
    bad.append("LVGL pool still 32 KB in sdkconfig.defaults")
if "CONFIG_LV_MEM_SIZE_KILOBYTES=36" in defaults:
    bad.append("LVGL pool still 36 KB in sdkconfig.defaults")
if "setup_release_unused()" not in ui:
    bad.append("build_setup does not release unused home/sleep objects first")
if "teardown_filament()" not in ui or "teardown_detail()" not in ui:
    bad.append("unused detail/filament pages are not torn down before setup")
if 'build_setup oom' not in ui or "return su_oom" not in ui:
    bad.append("build_setup does not null-check creates / log oom")
if "stay on home" not in ui:
    bad.append("build_setup failure does not fall back to home")

# Hint + setup not resident on home
if "create_qr_exit_hint()" not in ui or "destroy_qr_exit_hint()" not in ui:
    bad.append("QR hint create-on-repair / delete-on-exit missing")
if "build_setup();" in ui.split("void pet_ui_create")[-1][:800]:
    bad.append("pet_ui_create still eagerly builds setup pages")

# Inject is debug-only
if "config PET_DEBUG_STATUS_INJECT" not in kconfig:
    bad.append("CONFIG_PET_DEBUG_STATUS_INJECT missing from Kconfig")
if re.search(r"config PET_DEBUG_STATUS_INJECT.*?default y", kconfig, re.S):
    bad.append("PET_DEBUG_STATUS_INJECT must default n")
if "CONFIG_PET_DEBUG_STATUS_INJECT=y" in defaults or "CONFIG_PET_TEST_SHORT_EXPIRY=y" in defaults:
    bad.append("sdkconfig.defaults must not enable debug inject or test extras")

# command strings only inside the ifdef
def inside_inject_ifdef(src, needle):
    parts = re.split(r"#if CONFIG_PET_DEBUG_STATUS_INJECT \|\| CONFIG_PET_TEST_SHORT_EXPIRY", src)
    if len(parts) < 2:
        return False
    return needle in parts[1]

for needle in ("injjob", "injren", "injclr", "inject job", "inject rename", "inject clear"):
    if needle not in main:
        bad.append("%r missing from main.c" % needle)
    elif not inside_inject_ifdef(main, needle):
        bad.append("%r is not compiled only in debug/test inject ifdef" % needle)

print("heap inject contract:", "PASS" if not bad else "FAIL " + "; ".join(bad))
sys.exit(1 if bad else 0)
