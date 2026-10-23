#!/usr/bin/env bash
# 从 build/ 生成 GitHub Release 附件:应用镜像、完整镜像、网页刷机 zip、SHA256SUMS。
# 用法: tools/make_release.sh 4.7.2 [输出目录,默认 release/]
set -euo pipefail
ver="${1:?版本号,例如 4.7.1}"
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$root"
out="${2:-release}"; mkdir -p "$out"
app="build/FoloToy-AI-Passport.bin"; full="build/FoloToy-AI-Passport-full.bin"
[[ -f "$app" && -f "$full" ]] || { echo "先运行 tools/validate.sh --firmware" >&2; exit 1; }
name="ai-passport-bambu-pet-v${ver}"
cp "$app" "$out/${name}-app.bin"; cp "$full" "$out/${name}-full.bin"
wf="$(mktemp -d)/webflash-v${ver}"; mkdir -p "$wf"
cp -r webflash/index.html webflash/manifest.json webflash/manifest-update.json webflash/esp-web-tools "$wf/"
sed -i "s/v[0-9]\+\.[0-9]\+\.[0-9]\+/v${ver}/g; s/\"version\": \"[^\"]*\"/\"version\": \"${ver}\"/" "$wf/index.html" "$wf/manifest.json" "$wf/manifest-update.json"
sed -i "s/ai-passport-bambu-pet-v[0-9.]*-/${name}-/" "$wf/manifest.json" "$wf/manifest-update.json"
cp "$out/${name}-app.bin" "$out/${name}-full.bin" "$wf/"
python3 - "$wf" "$root/$out/webflash-v${ver}.zip" <<'PY'
import os, sys, zipfile
src, dst = sys.argv[1], sys.argv[2]; base = os.path.dirname(src)
with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as z:
    for d, _, fs in sorted(os.walk(src)):
        for f in sorted(fs):
            p = os.path.join(d, f); z.write(p, os.path.relpath(p, base))
PY
rm -rf "$(dirname "$wf")"
(cd "$out" && sha256sum "${name}-app.bin" "${name}-full.bin" "webflash-v${ver}.zip" > SHA256SUMS.txt && cat SHA256SUMS.txt)
