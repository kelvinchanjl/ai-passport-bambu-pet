#!/usr/bin/env python3
"""Build h2d_replay.jsonl from the REAL H2D push_all in H2D_ha-bambulab.json
(greghesp/ha-bambulab tests/pybambu/H2D.json @148c39a). Line = {"note", "msg"}; msg = MQTT payload {"print": {...}}.
Only the listed fields are edited; everything else is the real capture. No H2C capture was found publicly, H2D is closest
(same dual-nozzle device.extruder.info[] protocol)."""
import json, copy, os
H = os.path.dirname(os.path.abspath(__file__))
src = json.load(open(os.path.join(H, "H2D_ha-bambulab.json")))
full = src["push_all"]
L = []
def add(note, p): L.append({"note": note, "msg": None if p is None else {"print": p}})
add("REAL full push_all (gcode_state RUNNING, 0%)", copy.deepcopy(full))
add("REAL partial push_left_extruder (device.extruder.state=274 -> 左喷嘴 idx 1)", copy.deepcopy(src["push_left_extruder"]))
add("REAL partial push_door_opened", copy.deepcopy(src["push_door_opened"]))
add("partial: progress 42%, layer 11, 67 min left", {"mc_percent": 42, "layer_num": 11, "mc_remaining_time": 67, "command": "push_status"})
add("synth pause: gcode_state=PAUSE, print_error=0", {"gcode_state": "PAUSE", "print_error": 0})
add("resume: gcode_state=RUNNING", {"gcode_state": "RUNNING"})
add("synth runout via print_error=0x07008011 (AMS1 缺料) + PAUSE", {"gcode_state": "PAUSE", "print_error": 0x07008011})
add("clear: RUNNING, print_error=0, hms=[]", {"gcode_state": "RUNNING", "print_error": 0, "hms": []})
add("synth runout via hms only: attr=0x07007000 code=0x00020001 (AMS1 slot1 run out), still RUNNING", {"hms": [{"attr": 0x07007000, "code": 0x00020001}]})
add("clear hms", {"hms": [], "gcode_state": "RUNNING"})
add("synth runout AMS-HT via print_error=0x18008011 + PAUSE", {"gcode_state": "PAUSE", "print_error": 0x18008011})
add("clear", {"gcode_state": "RUNNING", "print_error": 0})
add("synth finish: gcode_state=FINISH, 100%", {"gcode_state": "FINISH", "mc_percent": 100, "mc_remaining_time": 0})
add("[按键 OK 确认完成]", None)
add("new print RUNNING", {"gcode_state": "RUNNING", "mc_percent": 3, "print_error": 0})
add("synth fail: gcode_state=FAILED, print_error=0x0300400C (task cancelled)", {"gcode_state": "FAILED", "print_error": 0x0300400C})
add("synth idle: gcode_state=IDLE", {"gcode_state": "IDLE", "print_error": 0})
f = copy.deepcopy(full); f["gcode_state"] = "FAILED"; f["print_error"] = 0x0C008001
add("synth fail on a FULL push_all copy (gcode_state=FAILED, print_error=0x0C008001)", f)
EXPECT = "PRINT PRINT PRINT PRINT PAUSE PRINT EMPTY PRINT EMPTY PRINT EMPTY PRINT DONE IDLE PRINT FAIL IDLE FAIL".split()
assert len(EXPECT) == len(L)
for x, e in zip(L, EXPECT): x["expect"] = e
with open(os.path.join(H, "h2d_replay.jsonl"), "w") as o:
    for x in L: o.write(json.dumps(x, ensure_ascii=False, separators=(",", ":")) + "\n")
print(len(L), "messages")
