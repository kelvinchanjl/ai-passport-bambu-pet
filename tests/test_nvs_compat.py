#!/usr/bin/env python3
"""Host test: build a v4.4.1-format NVS image with ESP-IDF's nvs_partition_gen (same keys/namespaces the v4.4.1 firmware wrote),
parse it back with tools/nvs_inspect.py, and run the current main/app_config.c against it. Also checks that an image whose
petcfg entries are erased (what factory reset leaves behind) is reported by nvs_inspect and makes the firmware fall back to setup."""
import os, subprocess, sys, tempfile
R = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
IDF = os.environ["IDF_PATH"]
sys.path.insert(0, os.path.join(R, "tools")); import nvs_inspect as NI  # noqa: E402
CSV = """key,type,encoding,value
petcfg,namespace,,
ssid,data,string,MyHomeWiFi
pass,data,string,secretpass123
uid,data,string,1234567890
token,data,string,{tok}
refresh,data,string,{ref}
sn,data,string,0000000000TEST1
host,data,string,cn.mqtt.bambulab.com
dname,data,string,My H2C
sns,data,string,0000000000TEST1
dnames,data,string,My H2C
dprods,data,string,H2C
texp,data,i64,1792000000
remday,data,i32,20376
petboot,namespace,,
count,data,u32,5
last,data,i32,1
""".format(tok="A" * 1100, ref="B" * 1100)

def cstr(s): return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'

def table(raw, erase_petcfg=False):
    if erase_petcfg:   # mark every petcfg data entry Erased (bitmap 0b00) like nvs_erase_all does
        raw = bytearray(raw); ns_idx = None
        part = NI.P.NVS_Partition("nvs", raw)
        for pg in part.pages:
            for e in pg.entries:
                if e.key == "petcfg" and e.metadata["namespace"] == 0: ns_idx = e.data["value"]
        for pg in part.pages:
            if pg.is_empty: continue
            for e in pg.entries:
                if not e.is_empty and e.metadata["namespace"] == ns_idx:
                    for j in range(e.index, e.index + e.metadata["span"]):
                        b = pg.start_address + 32 + j // 4; raw[b] &= ~(3 << (2 * (j % 4))) & 0xFF
    o = NI.analyse(bytes(raw))
    rows = []
    for ns, keys in o["namespaces"].items():
        for k in keys:
            if k["state"] != "LIVE": continue
            if k["type"] == "string":
                v = None
                part = NI.P.NVS_Partition("nvs", bytearray(raw))
                for pg in part.pages:
                    for e in pg.entries:
                        if not e.is_empty and e.key == k["key"] and e.state == "Written" and e.metadata["type"] == "string":
                            v = bytes(b for ch in e.children for b in ch.raw)[: e.data["size"]].rstrip(b"\0").decode()
                rows.append('{%s,%s,\'s\',%s,0}' % (cstr(ns), cstr(k["key"]), cstr(v)))
            elif k["value"] is not None:
                rows.append('{%s,%s,\'i\',NULL,%dLL}' % (cstr(ns), cstr(k["key"]), k["value"]))
    return o, '#include "nvs.h"\nconst nvs_stub_item_t NVS_STUB_ITEMS[] = {%s};\nconst size_t NVS_STUB_N = %d;\n' % (",\n".join(rows) or "{0}", len(rows))

def run(raw, mode, tmp):
    o, c = table(raw, mode == "erased")
    tc = os.path.join(tmp, "tbl_%s.c" % mode); open(tc, "w").write(c); exe = os.path.join(tmp, "t_" + mode)
    subprocess.check_call([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
                           "-I" + os.path.join(R, "tests", "nvs_stubs"), "-I" + os.path.join(R, "main"),
                           os.path.join(R, "tests", "test_nvs_compat.c"), os.path.join(R, "main", "app_config.c"), tc, "-o", exe])
    subprocess.check_call([exe, "ok" if mode == "ok" else "erased"])
    return o

if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as tmp:
        csv = os.path.join(tmp, "v441.csv"); out = os.path.join(tmp, "v441_nvs.bin"); open(csv, "w").write(CSV)
        subprocess.check_call([sys.executable, os.path.join(IDF, "components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"),
                               "generate", csv, out, "0x6000"], stdout=subprocess.DEVNULL)
        raw = open(out, "rb").read()
        o = run(raw, "ok", tmp)
        assert {"ssid", "uid", "token", "sn"} <= {k["key"] for k in o["namespaces"]["petcfg"] if k["state"] == "LIVE"}
        o = run(raw, "erased", tmp)
        assert not [k for k in o["namespaces"].get("petcfg", []) if k["state"] == "LIVE"]
        assert [k for k in o["namespaces"]["petboot"] if k["state"] == "LIVE"]
    print("test_nvs_compat: PASS")
