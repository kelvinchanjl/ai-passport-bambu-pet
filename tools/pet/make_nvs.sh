#!/usr/bin/env bash
# 生成桌宠配置 NVS 分区镜像(烧到 0x9000)。不要提交生成的 csv/bin:里面有密码和 token。
# 用法(云端): SSID=.. PASS=.. UID=.. TOKEN=.. SN=.. tools/pet/make_nvs.sh out/pet-nvs.bin
# 用法(方案 B 局域网): SSID=.. PASS=.. SN=.. LAN_IP=192.168.1.50 LAN_CODE=12345678 tools/pet/make_nvs.sh out/pet-nvs.bin
set -euo pipefail
out="${1:-pet-nvs.bin}"; csv="$(mktemp)"
{
  echo "key,type,encoding,value"
  echo "petcfg,namespace,,"
  echo "ssid,data,string,${SSID:?}"
  echo "pass,data,string,${PASS:-}"
  if [[ -n "${LAN_IP:-}" ]]; then
    echo "lan_ip,data,string,${LAN_IP}"
    echo "lan_code,data,string,${LAN_CODE:?}"
  else
    echo "uid,data,string,${UID_BAMBU:-${UID:?}}"
    echo "token,data,string,${TOKEN:?}"
  fi
  echo "sn,data,string,${SN:?}"
  echo "host,data,string,${HOST:-cn.mqtt.bambulab.com}"
} > "$csv"
python3 "${IDF_PATH:?source esp-idf/export.sh first}/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py" \
  generate "$csv" "$out" 0x6000
rm -f "$csv"
echo "wrote $out -> flash with: python3 -m esptool --chip esp32c3 -p /dev/cu.usbmodemXXXX write_flash 0x9000 $out"
