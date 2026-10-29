#!/usr/bin/env python3
"""v4.8 多组 Wi-Fi 的 NVS 占用,用 ESP-IDF 自己的 nvs_partition_gen(写)+ nvs_parser(读)实测,不靠手算:
给一个 petcfg(ssid/pass 两个字符串)加上 n 组 "wifis" blob(2 + 98n 字节),数分区里多出多少个 32 B 项。
期望 1 组 +6、3 组 +12、5 组 +18(见 main/wifi_slots.h)。同时确认 5 组 blob 放得进 24 KB NVS 分区(0x6000)而且旧键仍在。"""
import os, subprocess, sys, tempfile
R = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
IDF = os.environ["IDF_PATH"]
sys.path.insert(0, os.path.join(R, "tools")); import nvs_inspect as NI  # noqa: E402

def blob(n):
    return bytes([1, n]) + b"".join(("S%d" % i).encode().ljust(33, b"\0") + ("pw%d" % i).encode().ljust(65, b"\0") for i in range(n))

def used_entries(n, tmp):
    csv = "key,type,encoding,value\npetcfg,namespace,,\nssid,data,string,S0\npass,data,string,pw0\n"
    if n: csv += "wifis,data,hex2bin,%s\n" % blob(n).hex()
    c, o = os.path.join(tmp, "w%d.csv" % n), os.path.join(tmp, "w%d.bin" % n)
    open(c, "w").write(csv)
    subprocess.check_call([sys.executable, os.path.join(IDF, "components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"),
                           "generate", c, o, "0x6000"], stdout=subprocess.DEVNULL)
    raw = open(o, "rb").read()
    assert len(raw) == 0x6000
    part = NI.P.NVS_Partition("nvs", bytearray(raw))
    used = sum(1 + len(getattr(e, "children", None) or []) for pg in part.pages if not pg.is_empty for e in pg.entries if not e.is_empty)
    keys = {k["key"] for k in NI.analyse(raw)["namespaces"]["petcfg"] if k["state"] == "LIVE"}
    assert {"ssid", "pass"} <= keys and (n == 0 or "wifis" in keys)
    return used

if __name__ == "__main__":
    assert len(blob(5)) == 492
    with tempfile.TemporaryDirectory() as tmp:
        base = used_entries(0, tmp)
        got = {n: used_entries(n, tmp) - base for n in (1, 2, 3, 4, 5)}
    print("wifis blob extra NVS entries (IDF nvs_partition_gen + nvs_parser): %s (baseline %d)" % (got, base))
    assert got == {1: 6, 2: 9, 3: 12, 4: 15, 5: 18}, got
    print("test_wifi_blob_nvs_v48: PASS")
