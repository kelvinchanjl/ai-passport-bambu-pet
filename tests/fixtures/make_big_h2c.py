#!/usr/bin/env python3
"""v4.4.3: synthesize large compact H2C-like pushall reports (~14 KB and ~22 KB, 3-4 AMS) from the real H2D push_all.
Junk keys (not on the firmware whitelist) pad the message the way real H2C reports do (upgrade_state, net, xcam, ...)."""
import copy, json, os, sys
H = os.path.dirname(os.path.abspath(__file__))
p = json.load(open(os.path.join(H, "H2D_ha-bambulab.json")))["push_all"]
def make(target, n_ams):
    q = copy.deepcopy(p)
    units = q["ams"]["ams"]
    while len(units) < n_ams:
        u = copy.deepcopy(units[0]); u["id"] = str(len(units)); units.append(u)
    del units[n_ams:]
    q["upgrade_state"] = {"new_ver_list": [{"name": "ams%d" % i, "cur_ver": "00.00.06.49", "new_ver": "00.00.06.50", "sn": "X" * 15} for i in range(8)]}
    q["net"] = {"conf": 16, "info": [{"ip": 12345678, "mask": 4294967040}]}
    q["xcam"] = {"cfg": 1234, "buildplate_marker_detector": True, "halt_print_sensitivity": "medium"}
    i = 0
    while len(json.dumps({"print": q}, separators=(",", ":"), ensure_ascii=False).encode()) < target:
        q["pad_%03d" % i] = {"s": "esc\\\"aped,{]} 中文 " * 3, "a": [1, -2.5e3, True, False, None, {"x": [[], {}]}]}; i += 1
    return json.dumps({"print": q}, separators=(",", ":"), ensure_ascii=False)
for name, t, n in (("h2c_big_14k.json", 14 * 1024, 3), ("h2c_big_22k.json", 22 * 1024, 4)):
    s = make(t, n); open(os.path.join(H, name), "w").write(s); print(name, len(s.encode()), "B")
