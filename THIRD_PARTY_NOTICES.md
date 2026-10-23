# 第三方组件与素材说明

| 组件 | 用在哪里 | 许可证 |
| --- | --- | --- |
| [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) | `components/bsp/`(屏幕、按键、电池、音频驱动)、`main/key_guard.h`、`tools/verify_firmware.py`、工程框架 | MIT,Copyright (c) 2026 FoloToy |
| [ESP-IDF](https://github.com/espressif/esp-idf) 5.5.3 | 构建工具链与系统组件(不随仓库分发) | Apache-2.0 |
| [LVGL](https://lvgl.io) 9.5.0、espressif/esp_lvgl_port、button、esp_codec_dev | 构建时由组件管理器下载(`dependencies.lock`) | MIT / Apache-2.0 |
| [ESP Web Tools](https://github.com/esphome/esp-web-tools) 10.4.0 | `webflash/esp-web-tools/`(网页刷机) | Apache-2.0,见该目录 LICENSE |
| [Noto Sans CJK SC](https://github.com/notofonts/noto-cjk) Bold | `main/assets/font_pet_*.c`(按需裁剪的点阵字库) | SIL Open Font License 1.1 |
| [ha-bambulab](https://github.com/greghesp/ha-bambulab) | `tests/fixtures/H2D_ha-bambulab.json`(公开的 H2D 报文样例,已把序列号、IP、任务 ID 等换成占位符)及解析规则参考 | MIT,Copyright (c) 2023 ha-bambulab contributors |
| OpenBambuAPI、Bambuddy 等社区资料 | 拓竹云接口路径与报文格式参考 | 仅参考,未复制代码 |

## 美术素材

`art/pet/` 里的宠物「线线」(毛绒线团小怪兽)帧图和由它生成的 `main/assets/pet_frames.bin`
是用 AI 绘图工具生成后再人工修整的,采用 [CC BY-NC 4.0](https://creativecommons.org/licenses/by-nc/4.0/deed.zh-hans) 授权:
可以在非商业用途下使用和修改,需注明出处。代码部分仍按仓库根目录的 MIT 许可证。

部分帧(如暂停页的耗材盘)画有 "Bambu Lab" 字样的料盘贴纸,仅用于表示「拓竹打印机」的语境。

## 商标

Bambu Lab、拓竹、Bambu Handy、AMS 是 Bambu Lab 的商标;FoloToy、AI Passport 是 FoloToy 的商标。
本项目是个人开源项目,**与拓竹(Bambu Lab)及 FoloToy 官方无关**,未获得其认可或授权。
固件使用的拓竹云接口不是官方公开 API,随时可能变化或失效。
