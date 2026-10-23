# 从源码构建

## 环境

- **ESP-IDF v5.5.3**(目标芯片 `esp32c3`)。安装方法见 [乐鑫文档](https://docs.espressif.com/projects/esp-idf/zh_CN/v5.5.3/esp32c3/get-started/index.html)。
- 第一次构建时,组件管理器会按 `dependencies.lock` 下载 LVGL 9.5.0、esp_lvgl_port 2.9.0、button 4.2.0、esp_codec_dev 1.6.2 到 `managed_components/`。
- 主机测试需要 `cc` 和 Python 3;`test_ap_name_v452.py` 还要 Pillow、numpy、opencv-python。

## 构建

```bash
. $HOME/esp/esp-idf/export.sh          # 换成你的 ESP-IDF 路径
git clone <本仓库> && cd ai-passport-bambu-pet
idf.py set-target esp32c3              # 第一次需要;sdkconfig 会从 sdkconfig.defaults 生成
idf.py build
idf.py merge-bin -o FoloToy-AI-Passport-full.bin
```

产物:

| 文件 | 写入地址 | 说明 |
| --- | --- | --- |
| `build/FoloToy-AI-Passport.bin` | `0x10000` | 应用镜像(只更新应用用这个) |
| `build/FoloToy-AI-Passport-full.bin` | `0x0` | 完整镜像(bootloader + 分区表 + 应用) |
| `build/flash_args` | — | `esptool write_flash "@flash_args"` 用 |

直接烧到工牌:`idf.py -p <串口> flash`(写 bootloader、分区表和应用,不碰 NVS)。看日志:`idf.py -p <串口> monitor`。

## 测试和一键校验

```bash
tools/validate.sh --host       # 主机单元测试:状态判定、报文解析和回放、按键状态机、恢复出厂、NVS 兼容、热点名和二维码
tools/validate.sh --firmware   # 构建 + 合并镜像 + 检查分区布局和大小
tools/validate.sh              # 两个都跑
```

`test_ap_name_v452.py` 需要 `managed_components/`(先构建一次),如果 IDF 自带的 Python 里没有 opencv,可以用 `QR_PYTHON=/usr/bin/python3 tools/validate.sh --host` 指定另一个 Python。

## 打包发布附件

```bash
tools/validate.sh --firmware
tools/make_release.sh 4.7.1          # 生成 release/ 下的 app.bin、full.bin、webflash zip 和 SHA256SUMS.txt
```

`release/` 不进 git,把里面的文件上传到 GitHub Release。

## 内部测试版

```bash
idf.py -B build_test -D SDKCONFIG=build_test/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.test" build
```

测试版左上角有红色 `TEST` 标,登录 10 分钟就过期(用来走查到期提醒和重新扫码),串口会打印报文字段核对行,
还支持串口命令抓屏(`cap`、`demo1`、`demostep N` 等,见 `main/devcap.c`)。**不要发给普通用户。**

## 配置项(menuconfig → Desk Pet (Bambu))

`CONFIG_PET_WIFI_SSID`、`CONFIG_PET_BAMBU_TOKEN` 等只是 NVS 为空时的兜底值,默认全空。
**不要在这里填真实账号再提交或发布固件**,它们会被编进固件。正常使用请走手机配网。

## 换美术 / 改文字后重新生成资源

`main/assets/` 里是已经生成好的资源,构建时直接用。改了 `art/` 下的帧图或配网网页后:

```bash
python3 -m pip install pillow numpy fonttools
npm i -g lv_font_conv                         # 只有改了文字(要重新生成字库)才需要
python3 tools/pet/gen_assets.py --no-font     # 只重新生成帧图、图标、配网网页
python3 tools/pet/gen_assets.py               # 连字库一起(需要 Noto Sans CJK Bold,可用 PET_FONT_TTC 指定路径)
```

帧图规则见 `tools/pet/art.json`:纯 `#FF00FF` 像素在运行时换成当前耗材颜色,`#C000C0` 是侧面阴影(耗材色 ×0.7)。
脚本会检查资源总量不超过 5.5 MB。

## 代码导览

| 文件 | 内容 |
| --- | --- |
| `main/main.c` | 启动、模式切换(宠物 / 配网 / 重新登录 / 恢复出厂)、按键分发、主循环 |
| `main/pet_logic.c/h` | 纯逻辑(可主机测试):状态判定、锁存、自动切机、按键状态机、演示数据 |
| `main/pet_ui.c` | LVGL 界面:宠物动画、换色、详情页、耗材页、设置页、配网相关页面 |
| `main/status_bar.c` | 顶部时间和电量 |
| `main/bambu_cloud.c` | Wi-Fi + 拓竹云 MQTT、token 续期、多台订阅 |
| `main/json_filter.c`、`bambu_parse.c` | MQTT 分片流式过滤和报文解析(不缓冲整条 12–22 KB 的报文) |
| `main/bambu_api.c` | 拓竹 HTTPS 接口(验证码、登录、打印机列表、续期) |
| `main/setup_portal.c`、`net.c` | 热点、DNS 劫持、配网网页接口 |
| `main/app_config.c` | NVS 配置读写 |
| `main/devcap.c` | 仅测试版:串口抓屏 |
