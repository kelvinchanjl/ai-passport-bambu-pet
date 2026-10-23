# 刷机步骤

## 先选刷法

| 情况 | 用哪个文件 | 写到哪里 | 配网信息 |
| --- | --- | --- | --- |
| 第一次刷线线,或想彻底重来 | `ai-passport-bambu-pet-v4.7.1-full.bin`(完整镜像) | `0x0` | **会被清空**,刷完要重新扫码配网 |
| 工牌上已经是线线,只升级 | `ai-passport-bambu-pet-v4.7.1-app.bin`(只有应用) | `0x10000` | **保留**,刷完自动连上 |

文件都在 GitHub Release 里,`SHA256SUMS.txt` 里有校验值。

### 分区表(`partitions.csv`)

| 名称 | 类型 | 偏移 | 大小 | 内容 |
| --- | --- | --- | --- | --- |
| (bootloader) | — | `0x0` | — | 二级引导程序 |
| (partition table) | — | `0x8000` | `0xC00` | 分区表 |
| `nvs` | data/nvs | `0x9000` | `0x6000`(24 KB) | Wi-Fi、拓竹 token、打印机列表等配置 |
| `phy_init` | data/phy | `0xF000` | `0x1000` | 射频校准数据 |
| `factory` | app/factory | `0x10000` | `0x7F0000`(约 7.9 MB) | 固件应用 |

完整镜像是 `idf.py merge-bin` 合并出来的,从 `0x0` 一直覆盖到应用结尾,中间的 `nvs` 区会被填成 `0xFF`,所以会清掉配网信息。
只写 `0x10000` 的应用镜像不碰 `nvs`,配网信息就留着。

> 只更新应用的前提是工牌上现在跑的是线线固件(同一张分区表)。如果工牌上还是出厂固件或别的固件,请用完整镜像。

## 方法一:网页刷机(推荐)

刷机页是 [ESP Web Tools](https://github.com/esphome/esp-web-tools) 做的,已经内置在 zip 里,不依赖外网。

1. 下载 `webflash-v4.7.1.zip` 并解压。
2. 浏览器规定网页刷机只能在 https 或 localhost 下打开,直接双击 `index.html` 不行。在解压出的目录里开一个本地服务:
   ```bash
   cd webflash-v4.7.1
   python3 -m http.server 8000
   ```
3. 用电脑上的 **Chrome 或 Edge** 打开 <http://localhost:8000>(Safari、Firefox、手机浏览器都不支持 Web Serial)。
4. 用**能传数据**的 USB-C 线把工牌接到电脑。
5. 按需要点:
   - **「全新安装」**:写完整镜像。弹窗会问要不要先擦除整片 Flash,选不选都会清掉配网信息。
   - **「只更新应用」**:只写 `0x10000`,保留配网。
6. 在弹窗里选工牌的串口(名字一般带 `USB JTAG` 或 `usbmodem`,Windows 上是 `COMx`),点「安装」。
7. 进度走完(1–2 分钟)工牌会自动重启。刷完在终端按 `Ctrl+C` 关掉本地服务。

想放到网上给别人用:把 `webflash-v4.7.1/` 里的全部文件放到一个开了 GitHub Pages 的仓库(或任意 https 静态托管),打开对应网址即可,步骤同上。

## 方法二:esptool 命令行

```bash
python3 -m pip install --user esptool      # esptool v4.x 或 v5.x 都可以
```

找到串口:macOS 是 `/dev/cu.usbmodem*`,Linux 是 `/dev/ttyACM*`,Windows 在设备管理器里看 `COMx`。

**首次全量刷写**(会清空配网):

```bash
PORT=/dev/cu.usbmodem1101          # 换成你的串口
python3 -m esptool --chip esp32c3 -p "$PORT" -b 460800 \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0 ai-passport-bambu-pet-v4.7.1-full.bin
```

想彻底干净(比如之前刷过别的固件、NVS 里有旧数据),先擦一次再写:

```bash
python3 -m esptool --chip esp32c3 -p "$PORT" erase_flash
```

**只更新应用**(保留 Wi-Fi 和拓竹登录):

```bash
python3 -m esptool --chip esp32c3 -p "$PORT" -b 460800 \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x10000 ai-passport-bambu-pet-v4.7.1-app.bin
```

**用自己编译的产物分开写**(写 bootloader、分区表和应用三段,不碰 `0x9000` 的 NVS,配网会保留):

```bash
cd build
python3 -m esptool --chip esp32c3 -p "$PORT" -b 460800 write_flash "@flash_args"
# 即 0x0 bootloader/bootloader.bin  0x8000 partition_table/partition-table.bin  0x10000 FoloToy-AI-Passport.bin
```

写完后工牌会自动复位;没反应就拔插一次 USB。

## 如何保留配网

- 平时升级用「只更新应用」或 `0x10000` 的命令。
- 不要用 `0x0` 的完整镜像,不要 `erase_flash`。
- 分开写 bootloader / 分区表 / 应用(`@flash_args`)也不会动 `0x9000` 的 NVS,但分区表必须和本项目一致。
- 恢复出厂设置(见 [常见问题](faq.md))只清配网命名空间,不需要重新刷机。

## 刷不上怎么办

- 选不到串口:换一根能传数据的线,换个 USB 口(不要经过扩展坞)。
- 还是不行:按住工牌上的按键再插 USB 线,让芯片进下载模式后重试。
- Linux 提示没有权限:把自己加进 `dialout` 组(`sudo usermod -aG dialout $USER`,重新登录)。
- 刷完黑屏或反复重启:用完整镜像重刷一次;还不行先 `erase_flash` 再刷。
