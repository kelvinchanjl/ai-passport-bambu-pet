#!/usr/bin/env python3
"""v4.5.3 contract: factory reset is reachable only via settings -> 恢复出厂设置 -> confirm screen -> pet_reset state machine."""
import os, re
R = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
src = {f: open(os.path.join(R, "main", f), encoding="utf-8").read() for f in os.listdir(os.path.join(R, "main")) if f.endswith(".c")}
strip = lambda s: re.sub(r"//.*", "", s)
calls = [(f, m.start()) for f, s in src.items() for m in re.finditer(r"\bfactory_reset\(\)", strip(s))]
m = strip(src["main.c"])
assert len(calls) == 1 and calls[0][0] == "main.c", calls                      # only one call site
assert re.search(r"if \(pet_reset_tick\(&s_rst, now\) == PET_RST_EV_RESET\) factory_reset\(\);", m)
erasers = [f for f, s in src.items() for _ in re.finditer(r"\b(app_config_erase|nvs_erase_all|nvs_flash_erase\w*)\(", strip(s))]
assert sorted(erasers) == ["app_config.c", "app_config.c", "main.c"], erasers   # app_config_erase() def + its nvs_erase_all + the one call in factory_reset
assert "pet_reset_open(&s_rst, s_ok_held)" in m and "case SET_RESET:" in m
assert "pet_set_step(s_set_sel" in m and "% SET_COUNT" not in m                # no wrap-around
assert "s_ok_down_us" not in m and "RESET_ARM_MS" not in m                     # old hold-from-menu path gone
assert not re.search(r"bsp_button_\w*\(.*\).*reset", m, re.I)                  # no boot-time combo
print("factory reset contract: PASS")
