#!/usr/bin/env python3
"""Desk-pet asset pipeline. One command after swapping art:

    python3 tools/pet/gen_assets.py            # uses tools/pet/art.json, art root <repo>/art

Per state it picks the first existing source folder from art.json:
  * sprite folder (has meta.json): RGBA PNGs, any size/count. Solid pixels of filament_key_color
    (#FF00FF, in any state) become a separate A8 mask recolored at runtime (AMS colour); the 1px white
    outline stays in the base. meta: frame_ms, dx/dy, nozzle_tip [[x,y],...] per frame (or legacy
    nozzle_tip_in_sprite). Max sprite 150x200.
  * full-frame folder (no meta.json): 240x320 opaque PNGs; cropped to the pet band (placeholder art).
Outputs (main/assets/): pet_frames.bin (+ pet_frames.S incbin), pet_frames.h, heart.c, benchy.h,
font_pet_{14,18,24}.c, font_chars.txt.
"""
import argparse, glob, json, os, re, struct, subprocess, sys
from PIL import Image, ImageChops, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(ROOT, "main", "assets")
STATES = ["idle", "print", "done", "pause", "empty", "fail", "idle_stretch"]  # idle_stretch = 偶尔伸懒腰(不是独立状态,PET_ANIM_STRETCH)
BAND = (8, 58, 232, 270)

def px(img):
    return list(img.get_flattened_data() if hasattr(img, "get_flattened_data") else img.getdata())

def c565(r, g, b): return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)

def numbered(folder):
    fs = sorted(glob.glob(os.path.join(folder, "*.png")), key=lambda p: os.path.basename(p))
    fs = [f for f in fs if re.fullmatch(r"\d+\.png", os.path.basename(f))]
    return sorted(fs, key=lambda p: int(os.path.basename(p)[:-4]))

MAX_SPRITE = (150, 200)
WARNS = []   # 角色最大 150x200,下方留给进度/剩余时间/温度

def _key_split(im, key, path, shade_key=(0xC0, 0x00, 0xC0)):
    """Split RGBA into RGB565A8 base + A8 filament mask (#FF00FF) + A8 shade mask (#C000C0).
    Exact key pixels only (no antialias). Runtime: filament mask -> fil RGB; shade -> fil*0.7
    (brighten if near-black). No full-frame RAM buffer — masks live in flash like today."""
    base = bytearray(); alpha = bytearray(); mask = bytearray(); shade = bytearray()
    n_key = n_shade = n_near = 0
    for r, g, b, a in px(im):
        if (r, g, b) == key and a:
            base += struct.pack("<H", 0); alpha.append(0); mask.append(a); shade.append(0); n_key += 1
        elif shade_key and (r, g, b) == shade_key and a:
            base += struct.pack("<H", 0); alpha.append(0); mask.append(0); shade.append(a); n_shade += 1
        else:
            if a and r > 160 and b > 160 and g < 96: n_near += 1
            base += struct.pack("<H", c565(r, g, b)); alpha.append(a); mask.append(0); shade.append(0)
    if n_near:
        print("  WARN %s: %d near-magenta pixels not recoloured (antialiased key edge?)" % (os.path.relpath(path), n_near))
    return bytes(base + alpha), (bytes(mask) if n_key else None), (bytes(shade) if n_shade else None)


# ---- 离线缩放(仅当 art.json "scale" != 1;草稿用,正式版应由设计按目标尺寸导出) ----
SCALE = 1.0
DE = {"opaque": [], "all_src": [], "all_disp": []}
def _scaled_geom():
    W, H = MAX_SPRITE; nw, nh = round(W * SCALE), round(H * SCALE)
    return nw, nh, (W - nw) // 2, H - nh          # 以画布底边中点(脚)为缩放中心
def _scale_pt(x, y):
    nw, nh, px_, py_ = _scaled_geom(); return (round(px_ + x * nw / MAX_SPRITE[0]), round(py_ + y * nh / MAX_SPRITE[1]))
def _scaled_split(im, off, key, path):
    import numpy as np
    W, H = MAX_SPRITE; nw, nh, px_, py_ = _scaled_geom()
    cv = Image.new("RGBA", (W, H), (0, 0, 0, 0)); cv.paste(im, off)
    a = np.array(cv); keym = (a[..., 0] == key[0]) & (a[..., 1] == key[1]) & (a[..., 2] == key[2]) & (a[..., 3] > 0)
    mk = Image.fromarray(np.where(keym, a[..., 3], 0).astype("uint8"), "L")
    a[keym] = 0; base = Image.fromarray(a, "RGBA")
    base = base.convert("RGBa").resize((nw, nh), Image.LANCZOS).convert("RGBA")   # 预乘 alpha 再缩放,边缘不发黑
    mk = mk.resize((nw, nh), Image.LANCZOS)
    B = Image.new("RGBA", (W, H), (0, 0, 0, 0)); B.paste(base, (px_, py_)); M = Image.new("L", (W, H), 0); M.paste(mk, (px_, py_))
    bb = ImageChops.lighter(B.getchannel("A"), M).getbbox()
    B, M = B.crop(bb), M.crop(bb)
    _de_collect(B, M)
    data = bytearray(); alpha = bytearray()
    for (r, g, b, al) in px(B):
        data += struct.pack("<H", c565(r, g, b) if al else 0); alpha.append(al)
    mb = bytes(M.tobytes())
    return B, (bb[0], bb[1]), bytes(data + alpha), (mb if any(mb) else None)
def _q565(c):
    import numpy as np
    r, g, b = c[..., 0] >> 3, c[..., 1] >> 2, c[..., 2] >> 3
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1)   # 屏幕把 565 展开回 888 的方式
def _de_collect(B, M):
    """记录源像素 vs 屏幕显示(RGB565 存储 + 与黑底按 alpha 混合后再落到 RGB565)。换色区不计。"""
    import numpy as np
    a = np.array(B.convert("RGBA")).astype(np.int64)
    sel = a[..., 3] > 0
    if M is not None: sel &= np.array(M) == 0
    else: sel &= ~((a[..., 0] == 255) & (a[..., 1] == 0) & (a[..., 2] == 255))
    c = a[..., :3][sel]; al = a[..., 3][sel][:, None]
    q = _q565(c)
    op = al[:, 0] == 255
    DE["opaque"].append((c[op], q[op]))
    src = (c * al + 127) // 255; disp = _q565((q * al + 127) // 255)
    DE["all_src"].append(src); DE["all_disp"].append(disp)
def de_report():
    import numpy as np, deltae
    if not DE["opaque"]: return
    c = np.concatenate([x[0] for x in DE["opaque"]]); q = np.concatenate([x[1] for x in DE["opaque"]])
    d1 = deltae.de2000(c, q); d2 = deltae.de2000(np.concatenate(DE["all_src"]), np.concatenate(DE["all_disp"]))
    print("ΔE2000 source vs displayed (scale %.4f): opaque px n=%d mean %.3f p99 %.3f max %.3f | all px over black n=%d mean %.3f p99 %.3f max %.3f"
          % (SCALE, len(d1), d1.mean(), np.percentile(d1, 99), d1.max(), len(d2), d2.mean(), np.percentile(d2, 99), d2.max()))
    return d1, d2

def load_sprite(folder, default_ms):
    meta = json.load(open(os.path.join(folder, "meta.json"), encoding="utf-8"))
    kc = meta.get("filament_key_color", meta.get("key_color", "#FF00FF"))
    key = tuple(int(kc[i:i+2], 16) for i in (1, 3, 5))
    skc = meta.get("shade_key_color", meta.get("side_key", "#C000C0"))
    shade_key = tuple(int(skc[i:i+2], 16) for i in (1, 3, 5)) if skc else None
    files = numbered(folder); fr = []
    mframes = {}
    for k, f in enumerate(meta.get("frames") if isinstance(meta.get("frames"), list) else []):
        idx = f["frame"] if "frame" in f else (int(f["file"][:-4]) if "file" in f else k)   # 线线: {"file": "3.png", ...}
        mframes[idx] = f
    if tuple(meta.get("canvas", MAX_SPRITE)) != MAX_SPRITE:
        raise SystemExit("%s: canvas %s, expected %s" % (folder, meta.get("canvas"), MAX_SPRITE))
    for i, f in enumerate(files):
        im = Image.open(f).convert("RGBA"); w, h = im.size
        m = mframes.get(i, {})
        # 紧裁帧:offset = 裁剪块左上角在 150x200 画布上的位置;nozzle_tip 始终是画布坐标
        ox, oy = (int(v) for v in m.get("offset", (0, 0)))
        if ox < 0 or oy < 0 or ox + w > MAX_SPRITE[0] or oy + h > MAX_SPRITE[1]:
            raise SystemExit("%s: offset (%d,%d) + size %dx%d outside %dx%d canvas" % (f, ox, oy, w, h, *MAX_SPRITE))
        if "size" in m and tuple(m["size"]) != (w, h):
            raise SystemExit("%s: png is %dx%d but meta size %s" % (f, w, h, m["size"]))
        if SCALE == 1.0:
            data, mask, shade = _key_split(im, key, f, shade_key)
            _de_collect(im, None)
        else:
            im, (ox, oy), data, mask = _scaled_split(im, (ox, oy), key, f)
            shade = None  # SCALE!=1 drafts: shade key not scaled yet
            w, h = im.size
        fr.append(dict(w=w, h=h, cf="RGB565A8", data=data, mask=mask, shade=shade, dx=m.get("dx", 0), dy=m.get("dy", 0), off=(ox, oy)))
    # 出料口:新格式 nozzle_tip: [[x,y],...] 每帧一组(热端随动作晃);旧格式 nozzle_tip_in_sprite 单点。
    # 锚点 ax/ay = 各帧平均值 -> Benchy 固定不抖;每帧 tip 单独写出,挤出的线跟当前帧走。
    tips = meta.get("nozzle_tip")
    if tips is None and mframes and all("nozzle_tip" in v for v in mframes.values()):
        tips = [mframes[i]["nozzle_tip"] for i in range(len(files))]
    if tips is not None:
        if len(tips) != len(fr):
            raise SystemExit("%s: nozzle_tip has %d entries, %d frames" % (folder, len(tips), len(fr)))
        tips = [(int(x), int(y)) for x, y in tips]
        if SCALE != 1.0: tips = [_scale_pt(x, y) for x, y in tips]
        cropped = any("offset" in v for v in mframes.values())
        for (x, y), q in zip(tips, fr):
            cw, ch = MAX_SPRITE if cropped else (q["w"], q["h"])
            if not (0 <= x < cw and 0 <= y <= ch): raise SystemExit("%s: nozzle_tip (%d,%d) outside sprite" % (folder, x, y))
        ax = round(sum(t[0] for t in tips) / len(tips)); ay = round(sum(t[1] for t in tips) / len(tips))
    else:
        cropped = any("offset" in v for v in mframes.values())   # 紧裁帧默认锚点 = 画布底边中点
        dflt = [MAX_SPRITE[0] // 2, MAX_SPRITE[1]] if cropped else [fr[0]["w"] // 2, fr[0]["h"]]
        ax, ay = meta.get("nozzle_tip_in_sprite", meta.get("anchor", dflt))
        tips = [(ax, ay)] * len(fr)
    for q, t in zip(fr, tips): q["tip"] = t
    return dict(kind=1, frame_ms=meta.get("frame_ms", default_ms), ax=ax, ay=ay, bg=None, frames=fr,
                ty=meta.get("title_y", 34), iy=meta.get("info_y", 306))

def is_sprite(folder):
    mp = os.path.join(folder, "meta.json")
    if not os.path.exists(mp): return False
    m = json.load(open(mp, encoding="utf-8"))
    return "nozzle_tip_in_sprite" in m or "nozzle_tip" in m or isinstance(m.get("frames"), list)

def load_full(folder, default_ms):
    mp = os.path.join(folder, "meta.json")
    meta = json.load(open(mp, encoding="utf-8")) if os.path.exists(mp) else {}
    key = meta.get("filament_key_color")
    ims = [Image.open(f).convert("RGB").crop(BAND) for f in numbered(folder)]
    bg = ims[0].getpixel((2, 2)); bb = None
    for im in ims:
        d = ImageChops.difference(im, Image.new("RGB", im.size, bg)).convert("L").point(lambda v: 255 if v > 6 else 0).getbbox()
        if d: bb = d if bb is None else (min(bb[0], d[0]), min(bb[1], d[1]), max(bb[2], d[2]), max(bb[3], d[3]))
    x0, y0, x1, y1 = bb
    fr = []
    for im in ims:
        c = im.crop((x0, y0, x1, y1)); data = bytearray(); mask = bytearray(); has = False
        for r, g, b in px(c):
            k = 0.0
            if key:  # opaque art: estimate how much of the key magenta is mixed into this pixel
                k = max(0.0, min(1.0, (min(r, b) - g) / 255.0 * 1.15))
                if k < 0.3: k = 0.0
            if k > 0:
                has = True; mask.append(int(k * 255)); data += struct.pack("<H", c565(*bg) if k > 0.5 else c565(r, g, b))
            else:
                mask.append(0); data += struct.pack("<H", c565(r, g, b))
        fr.append(dict(w=c.width, h=c.height, cf="RGB565", mask=bytes(mask) if has else None, shade=None, dx=0, dy=0, data=bytes(data), tip=(0, 0), off=(0, 0)))
    return dict(kind=0, ty=meta.get("title_y", 34), iy=meta.get("info_y", 306), frame_ms=meta.get("frame_ms", default_ms), ax=BAND[0] + x0, ay=BAND[1] + y0, bg=bg, frames=fr)

def frames(art_root, cfg):
    global SCALE
    SCALE = float(cfg.get("scale", 1.0))
    blob = bytearray(); anims = []; all_frames = []
    for s in STATES:
        src = next((p if os.path.isabs(p) else os.path.join(art_root, p) for p in cfg["states"][s]
                    if numbered(p if os.path.isabs(p) else os.path.join(art_root, p))), None)
        if not src: raise SystemExit("no art for state " + s)
        a = (load_sprite if is_sprite(src) else load_full)(src, cfg["default_frame_ms"])
        a["first"] = len(all_frames); a["src"] = os.path.relpath(src, art_root)
        for f in a["frames"]:
            while len(blob) % 4: blob.append(0)
            off = len(blob); blob += f["data"]; moff = -1; soff = -1
            if f.get("mask"):
                while len(blob) % 4: blob.append(0)
                moff = len(blob); blob += f["mask"]
            if f.get("shade"):
                while len(blob) % 4: blob.append(0)
                soff = len(blob); blob += f["shade"]
            all_frames.append((f, off, moff, soff))
        anims.append(a)
        print("%-6s %-20s %s %d frames %dx%d %dms" % (s, a["src"], "sprite" if a["kind"] else "full", len(a["frames"]), a["frames"][0]["w"], a["frames"][0]["h"], a["frame_ms"]))
    os.makedirs(OUT, exist_ok=True)
    open(os.path.join(OUT, "pet_frames.bin"), "wb").write(blob)
    with open(os.path.join(OUT, "pet_frames.h"), "w") as f:
        f.write("// Generated by tools/pet/gen_assets.py from tools/pet/art.json. Do not edit.\n#pragma once\n#include <stdint.h>\n")
        f.write("// kind 0 = full-frame placeholder (x,y = top-left on screen, opaque RGB565, bg = screen colour)\n")
        f.write("// kind 1 = sprite (RGB565A8; mask A8=#FF00FF filament; shade_mask A8=#C000C0 shade)\n")
        f.write("typedef struct { int16_t w, h, dx, dy; int32_t off, mask_off, shade_mask_off; } pet_frame_desc_t;\n")
        f.write("typedef struct { uint8_t kind; uint16_t frame_ms; int16_t ax, ay; uint32_t bg; uint16_t first, count; int16_t title_y, info_y; } pet_anim_desc_t;\n// title_y/info_y = text centre lines from the art meta.json\n")
        f.write("static const pet_frame_desc_t PET_FRAMES[] = {\n")
        for fr, off, moff, soff in all_frames:
            f.write("    { %d, %d, %d, %d, %d, %d, %d },\n" % (fr["w"], fr["h"], fr["dx"], fr["dy"], off, moff, soff))
        f.write("};\n// Per-frame nozzle tip (sprite coords, same index as PET_FRAMES). Anim ax/ay = average of its frames' tips.\n")
        f.write("static const int16_t PET_TIPS[][2] = {\n")
        for fr, off, moff, soff in all_frames:
            f.write("    { %d, %d },\n" % fr["tip"])
        f.write("};\n// Per-frame crop offset: top-left of the (tight-cropped) frame on the 150x200 sprite canvas.\n")
        f.write("static const int16_t PET_OFFS[][2] = {\n")
        for fr, off, moff, soff in all_frames:
            f.write("    { %d, %d },\n" % fr["off"])
        f.write("};\nstatic const pet_anim_desc_t PET_ANIMS[%d] = {\n" % len(anims))
        for s, a in zip(STATES, anims):
            bg = a["bg"] or tuple(cfg.get("sprite_bg", (22, 24, 34)))
            f.write("    /* %-5s %s */ { %d, %d, %d, %d, 0x%02X%02X%02X, %d, %d, %d, %d },\n" % (s, a["src"], a["kind"], a["frame_ms"], a["ax"], a["ay"], *bg, a["first"], len(a["frames"]), a["ty"], a["iy"]))
        f.write("};\n")
    for s_, a in zip(STATES, anims):
        print("  %-12s %d frames %8d bytes" % (s_, len(a["frames"]), sum(len(q["data"]) + len(q.get("mask") or b"") + len(q.get("shade") or b"") for q in a["frames"])))
    if SCALE != 1.0: print("offline scale %.4f (LANCZOS, premultiplied alpha; mask scaled too)" % SCALE)
    de_report()
    nm = sum(1 for fr, off, moff, soff in all_frames if moff >= 0)
    ns = sum(1 for fr, off, moff, soff in all_frames if soff >= 0)
    print("frames blob: %d bytes (%d/%d filament masks, %d/%d shade masks)" % (len(blob), nm, len(all_frames), ns, len(all_frames)))

def benchy():
    """1-bit Benchy side silhouette, W x H, row-major, MSB first. Row 0 = top."""
    # Drawn on a 72x56 grid, scaled to W x H (model rises bed -> y=216, i.e. 46 px).
    W, H, S = 60, 46, 4
    im = Image.new("L", (72 * S, 56 * S), 0); d = ImageDraw.Draw(im)
    P = lambda pts: [(x * S, y * S) for x, y in pts]
    d.polygon(P([(2, 34), (70, 34), (64, 48), (56, 55), (14, 55), (6, 48)]), fill=255)       # hull
    d.polygon(P([(18, 34), (18, 16), (44, 16), (48, 34)]), fill=255)                         # cabin
    d.rectangle((16 * S, 12 * S, 48 * S, 16 * S), fill=255)                                  # roof
    d.rectangle((24 * S, 20 * S, 32 * S, 28 * S), fill=0)                                    # window
    d.rectangle((36 * S, 20 * S, 42 * S, 28 * S), fill=0)
    d.ellipse((50 * S, 0, 60 * S, 10 * S), fill=255); d.rectangle((51 * S, 5 * S, 59 * S, 34 * S), fill=255)  # chimney
    d.rectangle((53 * S, 2 * S, 57 * S, 8 * S), fill=0)
    im = im.resize((W, H), Image.LANCZOS).point(lambda v: 255 if v >= 128 else 0)
    rows = []
    for y in range(H):
        bits = bytearray((W + 7) // 8)
        for x in range(W):
            if im.getpixel((x, y)): bits[x // 8] |= 0x80 >> (x % 8)
        rows.append(bits)
    with open(os.path.join(OUT, "benchy.h"), "w") as f:
        f.write("// Generated by tools/pet/gen_assets.py. 1-bit Benchy silhouette, MSB first.\n#pragma once\n#include <stdint.h>\n")
        f.write("#define BENCHY_W %d\n#define BENCHY_H %d\n#define BENCHY_STRIDE %d\nstatic const uint8_t BENCHY_MASK[] = {\n" % (W, H, (W + 7) // 8))
        for r in rows: f.write("    " + ",".join("0x%02x" % v for v in r) + ",\n")
        f.write("};\n")
    return im

def heart():
    S = 4; W = H = 24
    im = Image.new("RGBA", (W * S, H * S), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    c = (255, 150, 170, 255)
    d.ellipse((2*S, 3*S, 13*S, 14*S), fill=c); d.ellipse((11*S, 3*S, 22*S, 14*S), fill=c)
    d.polygon([(2.6*S, 10.5*S), (21.4*S, 10.5*S), (12*S, 21.5*S)], fill=c)
    d.ellipse((6*S, 6*S, 9*S, 9*S), fill=(255, 220, 228, 255))
    im = im.resize((W, H), Image.LANCZOS)
    data = bytearray(); alpha = bytearray()
    for r, g, b, a in px(im):
        data += struct.pack("<H", c565(r, g, b)); alpha.append(a)
    raw = data + alpha
    with open(os.path.join(OUT, "heart.c"), "w") as f:
        f.write("// Generated by tools/pet/gen_assets.py. Do not edit.\n#include \"lvgl.h\"\nstatic const uint8_t heart_map[] = {\n")
        for i in range(0, len(raw), 24): f.write("    " + ",".join("0x%02x" % v for v in raw[i:i+24]) + ",\n")
        f.write("};\nconst lv_image_dsc_t img_heart = {\n  .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565A8, .w = %d, .h = %d, .stride = %d },\n  .data_size = sizeof(heart_map), .data = heart_map };\n" % (W, H, W * 2))

def font():
    lines = [l for l in open(os.path.join(HERE, "strings.txt"), encoding="utf-8") if not l.startswith("#")]
    allc = "".join(lines)
    chars = "".join(sorted(set(c for c in allc if ord(c) > 0x7E and not c.isspace())))
    big = "".join(sorted(set(c for c in "还剩小时分钟" )))
    otf = os.environ.get("PET_FONT_OTF", "/tmp/NotoSansSC-Bold.otf")   # Noto Sans SC Bold (SIL OFL 1.1)
    if not os.path.exists(otf):
        from fontTools.ttLib import TTCollection
        ttc = TTCollection(os.environ.get("PET_FONT_TTC", "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc"))
        next(t for t in ttc.fonts if "SC" in t["name"].getDebugName(1)).save(otf)
    conv = os.environ.get("LV_FONT_CONV", "lv_font_conv")   # npm i -g lv_font_conv
    # v4.2:14px 额外带 GB2312 一级汉字(3755 字),机器名/任务名里的常用中文能显示;缺字时固件改用型号兜底
    gb1 = "".join(bytes([hi, lo]).decode("gb2312") for hi in range(0xB0, 0xD8) for lo in range(0xA1, 0xFF)
                  if not (hi == 0xD7 and lo > 0xF9))
    extra14 = "".join(sorted(set(chars + gb1 + "（）【】「」《》！？，。：；、—…·～＿－＋＃")))
    T15 = "".join(sorted(set("帮我连上绑定成功登录过期过几天要重新扫码哦恢复出厂设置？")))
    T16 = "".join(sorted(set("连接中…未上拓竹云已要重新扫码啦读不到打印机数据")))   # v4.4.3 +「读不到打印机数据」
    T13 = "".join(sorted(set("按中键重新扫码登录还剩天分钟继续住秒")))
    for size, rng, sym in ((18, "0x20-0x7E", chars), (14, "0x20-0x7E", extra14), (12, "0x20-0x7E", chars), (24, "0x20,0x30-0x3A,0x25", big), (36, "0x2D,0x30-0x3A", ""), (8, "0x4C,0x52", ""), (15, "0x20-0x7E", T15), (16, "0x20-0x7E", T16), (13, "0x20-0x7E", T13), (10, "0x30-0x39", "")):  # v4.5:15/16/13 带配网页文字;16 缺字回退 14(机器名)  # v4.4.1:16px 只有「连接中…」「未连上拓竹云」  # v4.4.1:15px 只有数字和 -(详情页温度当前值)  # v4.3:8px 只有 L/R,耗材页在用标签

        name = "font_pet_%d" % size; p = os.path.join(OUT, name + ".c")
        subprocess.check_call([conv, "--font", otf, "-r", rng] + (["--symbols", sym] if sym else []) + ["--size", str(size), "--bpp", "4",
                               "--format", "lvgl", "--no-compress", "--lv-font-name", name, "-o", p])
        if size == 16:   # v4.5:绑定页「已连上 <机器名>」:16px 没有的字回退到 14px(GB2312 一级)
            t = open(p, encoding="utf-8").read()
            t = t.replace(".fallback = NULL", ".fallback = &font_pet_14").replace("#if LVGL_VERSION_MAJOR", "LV_FONT_DECLARE(font_pet_14);\n#if LVGL_VERSION_MAJOR", 1)
            open(p, "w", encoding="utf-8").write(t)
        t = open(p).read()
        t = re.sub(r'#ifdef LV_LVGL_H_INCLUDE_SIMPLE.*?#endif', '#include "lvgl.h"', t, count=1, flags=re.S)
        open(p, "w").write(t)
    open(os.path.join(OUT, "font_chars.txt"), "w", encoding="utf-8").write(chars + "\n" + big + "\n")
    print("font glyphs (non-ASCII):", len(chars))

def icons():
    """v4.2 详情页卡片图标(A8,16x18,运行时着白色):喷嘴 / 热床 / 仓温。4x 画再缩小,边缘有抗锯齿。"""
    from PIL import ImageDraw
    W, H, K = 16, 18, 4
    def mk(draw_fn):
        im = Image.new("L", (W * K, H * K), 0); draw_fn(ImageDraw.Draw(im), K)
        return im.resize((W, H), Image.LANCZOS)
    def nozzle(d, k):   # 热端方块 + 下方喷嘴尖
        d.rounded_rectangle((2 * k, 1 * k, 13 * k, 9 * k), radius=k, outline=255, width=2 * k)
        d.polygon([(4 * k, 10 * k), (11 * k, 10 * k), (7.5 * k, 17 * k)], fill=255)
    def bed(d, k):      # 三道热浪 + 底板
        for i in range(3):
            x = (3 + i * 5) * k
            d.arc((x - 2 * k, 1 * k, x + 2 * k, 6 * k), 90, 270, fill=255, width=int(1.6 * k))
            d.arc((x - 2 * k, 5 * k, x + 2 * k, 10 * k), 270, 90, fill=255, width=int(1.6 * k))
        d.rounded_rectangle((0, 13 * k, 16 * k - 1, 16 * k), radius=k, fill=255)
    def chamber(d, k):  # 机箱 + 温度计
        d.rounded_rectangle((1 * k, 1 * k, 15 * k, 17 * k), radius=2 * k, outline=255, width=2 * k)
        d.line((4 * k, 12 * k, 12 * k, 12 * k), fill=255, width=2 * k)
        d.ellipse((6.5 * k, 4 * k, 9.5 * k, 7 * k), fill=255)
    out = ['#include "lvgl.h"', "// generated by tools/pet/gen_assets.py icons() — v4.2 detail-page card icons (A8)"]
    for name, fn in (("nozzle", nozzle), ("bed", bed), ("chamber", chamber)):
        im = mk(fn); im.save(os.path.join(OUT, "ico_%s.png" % name))
        data = bytes(px(im))
        out.append("static const uint8_t ico_%s_map[] = {%s};" % (name, ",".join("0x%02x" % b for b in data)))
        out.append("const lv_image_dsc_t img_ico_%s = { .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8, "
                   ".w = %d, .h = %d, .stride = %d }, .data_size = %d, .data = ico_%s_map };" % (name, W, H, W, len(data), name))
    open(os.path.join(OUT, "icons.c"), "w").write("\n".join(out) + "\n")

def portal(src):
    import gzip
    html = open(src, encoding="utf-8").read()
    # First-time setup needs internet before /api/bind: send Wi-Fi with the code request.
    # Older portal.html builds lacked Wi-Fi creds on /api/code; patch only if they are still missing.
    old = "post('/api/code',{account:a})"
    if "ssid:$('ssid').value,password:$('pw').value" in html:
        pass  # designer copy already sends ssid/password: never double-apply
    elif old in html:
        html = html.replace(old, "post('/api/code',RE?{account:a}:{account:a,ssid:$('ssid').value,password:$('pw').value})")
        print("portal: patched /api/code to send Wi-Fi creds")
    else:
        raise SystemExit("portal.html: /api/code call not recognised; check the API contract")
    # v3.3 方案 B:portal 必须带"高级 · 用局域网连接"并 POST /api/bind_lan(固件 h_bind_lan 对应)
    if "post('/api/bind_lan'" not in html or html.count("'/api/bind_lan'") != 1:
        raise SystemExit("portal.html: expected exactly one POST /api/bind_lan (高级 · 用局域网连接)")
    html = portal_multi(html)
    html = re.sub(r"<title>[^<]*</title>", "<title>线线配网</title>", html, count=1)   # v4.5:只改网页标题;热点名仍是 Passport-XXXX
    html = html.replace("看工牌上的小打印头蹦起来，就是成功了 🎉", "线线收到啦，正在连上拓竹云～")   # v4.4.3 配网成功文案(UI 定稿)
    data = gzip.compress(html.encode("utf-8"), 9, mtime=0)
    open(os.path.join(OUT, "portal.html.gz"), "wb").write(data)
    print("portal: %d -> %d bytes gz" % (len(html.encode()), len(data)))

def portal_multi(html):
    """v4.2: 「选择打印机」从单选下拉框改成复选列表(最多 4 台),POST /api/bind 带 device_ids 数组。"""
    sel = '<select id="dev" disabled><option>登录后显示</option></select>'
    if 'id="devs"' in html: return html              # 设计稿已经是多选版
    if sel not in html: raise SystemExit("portal.html: 打印机下拉框没找到,无法打多选补丁")
    html = html.replace(sel, '<div id="devs" class="devs"><div class="devhint">登录后显示</div></div>'
                        '<div class="devhint" id="devcnt">可多选，最多 4 台；第一台开机默认显示</div>')
    style = ('<style>.devs{display:flex;flex-direction:column;gap:8px;margin-top:6px}'
             '.devs label{display:flex;align-items:center;gap:10px;padding:10px 12px;border-radius:10px;background:rgba(127,127,127,.12);font-size:15px}'
             '.devs input{width:20px;height:20px;accent-color:#00AE42}.devs .on{width:8px;height:8px;border-radius:4px;background:#00AE42;margin-left:auto}'
             '.devs .off{width:8px;height:8px;border-radius:4px;background:#888;margin-left:auto}.devhint{font-size:12px;opacity:.6;margin-top:6px}</style>')
    html = html.replace("</head>", style + "</head>", 1)
    old_fill = "let d=$('dev');d.disabled=0;d.innerHTML=(r.devices||[]).map(x=>`<option value=\"${x.id}\">${x.name}</option>`).join('');"
    old_fill = old_fill.replace('\\"', '"')
    if old_fill not in html: raise SystemExit("portal.html: 打印机列表填充代码没找到")
    new_fill = ("let d=$('devs'),L=r.devices||[];d.innerHTML=L.length?L.map((x,i)=>`<label><input type=checkbox value=\"${x.id}\" ${i<1?'checked':''}>"
                "<span>${x.name}</span><i class=${x.online?'on':'off'}></i></label>`).join(''):'<div class=devhint>这个账号下没有打印机</div>';"
                "d.onchange=e=>{let c=[...d.querySelectorAll('input:checked')];if(c.length>4){e.target.checked=false;err('最多选 4 台')}};")
    new_fill = new_fill.replace('\\"', '"')
    html = html.replace(old_fill, new_fill)
    old_b = "device_id:$('dev').value}"
    if old_b not in html: raise SystemExit("portal.html: /api/bind 的 device_id 没找到")
    html = html.replace(old_b, "device_ids:[...$('devs').querySelectorAll('input:checked')].map(x=>x.value)}")
    if "$('dev')" in html: raise SystemExit("portal.html: 还有旧的 $('dev') 引用")
    print("portal: patched printer picker -> checkboxes (max 4)")
    return html

ASSET_LIMIT = 5767168   # 5.5 MiB: pet art + embedded portal + fonts, leaves room for code in the 8MB factory slot

def total_report():
    sizes = {"pet_frames.bin": os.path.getsize(os.path.join(OUT, "pet_frames.bin")),
             "portal.html.gz": os.path.getsize(os.path.join(OUT, "portal.html.gz"))}
    for n in sorted(os.listdir(OUT)):   # generated C arrays: count the bytes they compile to (0x.. literals)
        if n.endswith(".c") or n == "benchy.h":
            sizes[n] = len(re.findall(r"0x[0-9a-fA-F]{2}\b", open(os.path.join(OUT, n), encoding="utf-8").read()))
    total = sum(sizes.values())
    for n, v in sizes.items(): print("  %-16s %9d" % (n, v))
    print("total assets: %d / %d bytes (%.1f%%)" % (total, ASSET_LIMIT, 100.0 * total / ASSET_LIMIT))
    if total > ASSET_LIMIT: raise SystemExit("asset budget exceeded: %d > %d bytes" % (total, ASSET_LIMIT))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--art-root", default=os.path.join(ROOT, "art"))
    ap.add_argument("--config", default=os.path.join(HERE, "art.json"))
    ap.add_argument("--no-font", action="store_true")
    a = ap.parse_args()
    frames(a.art_root, json.load(open(a.config, encoding="utf-8")))
    benchy(); heart(); icons(); portal(os.path.join(a.art_root, "setup", "portal.html"))
    if not a.no_font: font()
    total_report()

