#!/usr/bin/env python3
"""v4.5.2 contract: a page/printer switch that keeps the same pet state (s_reapply) must apply the state directly,
not run the full-screen black fade (start_transition), which made the screen flash on every page switch."""
import os, re
R = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
s = open(os.path.join(R, "main", "pet_ui.c"), encoding="utf-8").read()
m = re.search(r"if \(s_reapply && st == s_cur && st == s_pending && !s_fading\) \{ s_reapply = false; apply_state\(st\); \}\s*\n\s*else if \(st != s_pending", s)
assert m, "same-state reapply must bypass start_transition()"
for fn in ("pet_ui_show_detail", "pet_ui_show_filament"):
    body = s[s.index("void %s(" % fn):]; body = body[: body.index("\n}\n")]
    assert "start_transition" not in body and "s_fade" not in body, fn
print("no-flash page switch contract: PASS")
