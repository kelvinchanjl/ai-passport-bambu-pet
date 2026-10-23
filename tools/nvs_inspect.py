#!/usr/bin/env python3
"""Read-only analysis of a badge NVS dump (esptool read_flash 0x9000 0x6000 nvs.bin).
For every namespace (petcfg / petboot / petjob / nvs.net80211 ...) lists each key as LIVE (Written) or ERASED,
with type and value length (secrets are never printed). Also reports page states, so you can tell apart:
  * petcfg keys all ERASED          -> config was cleared on the device (factory reset = nvs_erase_all)
  * no FREE/EMPTY page               -> nvs_flash_init would return NO_FREE_PAGES
  * pages with bad CRC / Invalid     -> NVS corruption (the IDF loader may drop or erase that page)
Usage: python3 tools/nvs_inspect.py nvs.bin [--json]"""
import json, os, sys
IDF = os.environ["IDF_PATH"]
sys.path.insert(0, os.path.join(IDF, "components", "nvs_flash", "nvs_partition_tool"))
import nvs_parser as P  # noqa: E402
SECRET = {"token", "refresh", "pass", "lan_code"}

def analyse(raw):
    part = P.NVS_Partition("nvs", bytearray(raw))
    ns_names = {0: "<ns-table>"}
    pages, items = [], []
    for pg in part.pages:
        h = pg.header
        pages.append({"addr": pg.start_address, "status": "EMPTY" if pg.is_empty else h["status"], "seq": h["page_index"],
                      "crc_ok": pg.is_empty or h["crc"]["original"] == h["crc"]["computed"]})
        if pg.is_empty: continue
        for e in pg.entries:
            if e.is_empty or e.key is None: continue
            m = e.metadata
            if m["namespace"] == 0 and e.state == "Written" and isinstance(e.data, dict) and "value" in e.data:
                ns_names[e.data["value"]] = e.key
            items.append((pg.start_address, e.state, m["namespace"], e.key, m["type"], e.data))
    out = {"pages": pages, "namespaces": {}}
    for addr, st, ns, key, ty, data in items:
        if ns == 0: continue
        name = ns_names.get(ns, "ns#%d" % ns)
        size = data.get("size") if isinstance(data, dict) else None
        val = None
        if isinstance(data, dict) and "value" in data and key not in SECRET and ty not in ("string", "blob_data", "blob"):
            val = data["value"]
        out["namespaces"].setdefault(name, []).append(
            {"key": key, "state": "LIVE" if st == "Written" else st.upper(), "type": ty, "size": size, "value": val, "page": hex(0x9000 + addr)})
    return out

def report(o):
    print("pages:", ", ".join("%s %s seq=%s%s" % (hex(0x9000 + p["addr"]), p["status"], p["seq"], "" if p["crc_ok"] else " BAD-CRC") for p in o["pages"]))
    free = sum(1 for p in o["pages"] if p["status"] in ("EMPTY", "Empty"))
    print("free pages: %d%s" % (free, "  <-- nvs_flash_init would fail with NO_FREE_PAGES" if free == 0 else ""))
    for ns, keys in o["namespaces"].items():
        live = {}
        for k in keys:                       # last write wins
            if k["state"] == "LIVE": live[k["key"]] = k
        erased = sorted({k["key"] for k in keys if k["state"] != "LIVE"} - set(live))
        print("[%s] live: %s" % (ns, ", ".join("%s(%s%s)" % (k, v["type"], "" if v["value"] is None else "=%s" % v["value"]) for k, v in sorted(live.items())) or "-"))
        if erased: print("[%s] erased only: %s" % (ns, ", ".join(erased)))
    cfg = {k["key"] for k in o["namespaces"].get("petcfg", []) if k["state"] == "LIVE"}
    need = {"ssid", "uid", "token", "sn"}
    print("verdict: petcfg %s" % ("OK (firmware should load it)" if need <= cfg else
          "MISSING live keys %s -> firmware will start the setup portal" % sorted(need - cfg)))

if __name__ == "__main__":
    o = analyse(open(sys.argv[1], "rb").read())
    print(json.dumps(o, indent=1, default=str)) if "--json" in sys.argv else report(o)
