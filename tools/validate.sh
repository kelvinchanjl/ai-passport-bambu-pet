#!/usr/bin/env bash
# 主机单元测试 + 固件构建。用法: tools/validate.sh [--all|--host|--firmware]
# 依赖: cc、python3、ESP-IDF 5.5.3(已 source export.sh)。
# test_ap_name_v452.py 需要 managed_components/(第一次 idf.py build 后自动下载)和 Pillow + numpy + opencv-python;
# 如果 IDF 的 Python 里没有 opencv,用 QR_PYTHON=/path/to/python3 指定另一个解释器。
set -euo pipefail
mode="${1:---all}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

run_host_tests() {
    : "${IDF_PATH:?请先 source ESP-IDF 5.5.3 的 export.sh}"
    local t; t="$(mktemp -d /tmp/pet-host-tests.XXXXXX)"
    local CC="${CC:-cc}"
    local W=(-std=c11 -Wall -Wextra -Werror -Imain)
    local cjson="${IDF_PATH}/components/json/cJSON"
    "$CC" "${W[@]}" tests/test_pet_logic.c main/pet_logic.c -o "$t/pet_logic" && "$t/pet_logic"
    "$CC" "${W[@]}" -DSTATUS_BAR_PURE_ONLY tests/test_status_bar_v47.c main/status_bar.c -o "$t/status_bar" && "$t/status_bar"
    "$CC" "${W[@]}" -I"$cjson" tests/test_replay.c main/bambu_parse.c main/pet_logic.c "$cjson/cJSON.c" -lm -o "$t/replay"
    "$t/replay" tests/fixtures/h2d_replay.jsonl
    "$CC" "${W[@]}" -I"$cjson" tests/test_fields_v42.c main/bambu_parse.c main/pet_logic.c "$cjson/cJSON.c" -lm -o "$t/fields"
    "$t/fields" tests/fixtures/H2D_ha-bambulab.json
    "$CC" "${W[@]}" -I"$cjson" tests/test_ams_v43.c main/bambu_parse.c main/pet_logic.c "$cjson/cJSON.c" -lm -o "$t/ams"
    "$t/ams" tests/fixtures/H2D_ha-bambulab.json
    "$CC" "${W[@]}" -I"$cjson" tests/test_v441.c main/bambu_parse.c main/pet_logic.c "$cjson/cJSON.c" -lm -o "$t/v441" && "$t/v441"
    "$CC" "${W[@]}" -I"$cjson" tests/test_stream_v443.c main/json_filter.c main/bambu_parse.c main/pet_logic.c "$cjson/cJSON.c" -lm -o "$t/stream"
    "$t/stream" tests/fixtures/h2c_big_14k.json tests/fixtures/h2c_big_22k.json
    "$CC" "${W[@]}" tests/test_keys_v44.c main/pet_logic.c -o "$t/keys" && "$t/keys"
    "$CC" "${W[@]}" tests/test_reset_v453.c main/pet_logic.c -o "$t/reset" && "$t/reset"
    "$CC" "${W[@]}" tests/test_key_guard.c -o "$t/key_guard" && "$t/key_guard"
    "$CC" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src tests/test_bsp_display_rounding.c components/bsp/src/bsp_display_rounding.c -o "$t/rounding" && "$t/rounding"
    "$CC" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src tests/test_bsp_es8311_sleep_check.c components/bsp/src/bsp_es8311_sleep_check.c -o "$t/es8311" && "$t/es8311"
    "$CC" -std=c11 -Wall -Wextra -Werror -Itests/bsp_stubs -Icomponents/bsp/include tests/test_bsp_button.c -o "$t/button" && "$t/button"
    "$CC" -std=c11 -Wall -Wextra -Werror -Itests/bsp_stubs -Icomponents/bsp/include tests/test_bsp_lvgl_init.c components/bsp/src/bsp_display_rounding.c -o "$t/lvgl_init" && "$t/lvgl_init"
    "$CC" -std=c11 -Wall -Wextra -Werror -Itests/audio_stubs -Icomponents/bsp/include -Icomponents/bsp/src tests/test_bsp_audio_recovery.c components/bsp/src/bsp_es8311_sleep_check.c -o "$t/audio" && "$t/audio"
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_cpu_contract_v441.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_nvs_compat.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_no_flash_v452.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_reset_contract_v453.py
    if [[ -d managed_components/lvgl__lvgl ]]; then
        PYTHONDONTWRITEBYTECODE=1 "${QR_PYTHON:-python3}" tests/test_ap_name_v452.py
    else
        echo "SKIP test_ap_name_v452(先 idf.py build 一次,下载 managed_components)"
    fi
    rm -rf "$t"
    echo "Host tests: PASS"
}

run_firmware_build() {
    command -v idf.py >/dev/null || { echo "ERROR: 先 source ESP-IDF 5.5.3 的 export.sh" >&2; return 1; }
    idf.py -B build build
    idf.py -B build merge-bin -o FoloToy-AI-Passport-full.bin
    python3 tools/verify_firmware.py build
    echo "Firmware build: PASS -> build/FoloToy-AI-Passport.bin (@0x10000), build/FoloToy-AI-Passport-full.bin (@0x0)"
}

case "${mode}" in
    --all) run_host_tests; run_firmware_build ;;
    --host) run_host_tests ;;
    --firmware) run_firmware_build ;;
    *) echo "Usage: $0 [--all|--host|--firmware]" >&2; exit 2 ;;
esac
