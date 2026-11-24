# Cardputer-Adv 刷写、Amiibo 准备与 Switch 实机验证指南

## 1. 刷写固件到 Cardputer-Adv

> [!IMPORTANT]
> 本固件通过 `-DARDUINO_USB_MODE=0 -DARDUINO_USB_CDC_ON_BOOT=0` 将 Type-C 口专用于 Switch Pro HID 枚举，因此刷写时**必须手动进入 ESP32-S3 ROM Download 模式**（每次重刷均需按住 G0 上电）。

### 方法 A：命令行 `esptool` 烧录合并镜像（推荐）
1. **关闭 Cardputer-Adv 侧边电源开关**（或拔掉 USB-C 线）。
2. **按住机身内部/顶部 `G0` 按键不放**，插入 USB-C 数据线连接电脑，1 秒后松开 `G0`。
3. 确认设备管理器/串口出现 `USB JTAG/serial debug unit`（如 `COMx`）。
4. 使用单文件合并固件从 `0x0` 地址整片烧录：
   ```bash
   python -m esptool --chip esp32s3 --port COMx --baud 460800 write_flash -z 0x0 dist/cardputer-adv-amiibo-<version>-merged-0x0.bin
   ```
   或直接用 PlatformIO：
   ```bash
   pio run -t upload
   ```
5. 烧录完成后**拔插一次 USB-C 或拨动复位/电源开关**正常启动，屏幕应亮起 `NO-USB` / `ATTACH` 仪表盘与当前 Git 版本号。

### 方法 B：M5Burner 自定义烧录
选择合并包 `cardputer-adv-amiibo-<version>-merged-0x0.bin`，烧录起始地址填 **`0x0`**，Flash 模式 `DIO` / `80MHz` / `8MB`。

---

## 2. 下载并准备 Amiibo 数据（540 字节 `.bin` / Flipper `.nfc`）

本项目自带下载校验脚本 `tools/fetch_amiibo.py`，直接从开源仓库 `AmiiboDB/Amiibo` 拉取标准 540 字节 NTAG215 镜像（并自动校验/修复 `BCC0`/`BCC1` 校验字节），也可结合 `amiiboapi.com` 查询角色：

```bash
# 搜索并下载塞尔达林克相关 Amiibo 到 sd_stage/amiibo/
python tools/fetch_amiibo.py --search "Link" --out sd_stage/amiibo --limit 5

# 搜索马力欧或动物森友会居民卡
python tools/fetch_amiibo.py --search "Isabelle" --out sd_stage/amiibo --limit 5

# 仅检索清单与 AmiiboAPI 元数据
python tools/fetch_amiibo.py --search "Zelda" --list-only --api-info
```

将下载后的 `sd_stage/amiibo/` 文件夹整夹复制到 FAT32 microSD 卡根目录的 `/amiibo/` 下（支持多级子文件夹；也兼容 Flipper Zero `.nfc` 文本文件）。即使暂不插 SD 卡，开机按 **`Space`** 或 **`G0`** 也可加载内置的 `Demo Blank Tag` 先验证 Switch 识别和 NFC 读卡链路。

---

## 3. Cardputer-Adv 按键操作说明

### 全局快捷键（任意界面均有效）
- **`Tab`**：在 **SD 文件浏览器** 与 **手柄实时控制面板** 之间切换。
- **`Space` 或硬件 `G0` 按键**：快速 **放卡 / 移卡**（Switch 上对应把 Amiibo 贴上或移开 NFC 感应区）。
- **`R`**：立即为当前内存中的 Amiibo 生成一组新的 **随机 7 字节 UID**（绕过游戏每日同卡仅刷 1 次限制）。
- **`U`**：开关 **每次读卡后自动随机 UID**（`AUTO-RND` 模式；开启后将不向 SD 卡回写游戏存档）。

### SD 浏览器模式（`Tab` 第 1 页）
- **`;` / `.`（方向上/下）**：上下移动选中文件或目录。
- **`,` / `/`（方向左/右）**：向上/向下快速翻页（每次 5 项）。
- **`Enter`**：进入子目录，或读取选中 `.bin`/`.nfc` 并**自动放卡就绪（`TAG:ON`）**。
- **`Backspace`**：若当前已放卡则先移卡（`TAG:--`）；否则返回上级目录。
- **`S`**：重新扫描 microSD 卡（支持热插卡）。

### 手柄直通模式（`Tab` 第 2 页，方便单机操作 Switch）
- **`W A S D`**：左摇杆上下左右
- **`; . , /`**：十字键 Up / Down / Left / Right
- **`L` / `K` / `I` / `J`**：Switch **`A` / `B` / `X` / `Y`**
- **`Q` / `E`**：`L` / `R`（在 Switch“更改握法/顺序”页面同时按 `Q` + `E` 配对！）
- **`1` / `2`**：`ZL` / `ZR`
- **`-` / `=`**：`Minus (-)` / `Plus (+)`
- **`H`**：`HOME` 键

---

## 4. 电脑端预检（可选） & Switch 实机验证步骤

### 电脑端 USB 预检
将已刷好固件的 Cardputer-Adv 插入 Windows/macOS/Linux 电脑（不要按 G0）：
1. 在 Windows 设备管理器“人体学输入设备 / 游戏控制器”中应能看到 `Pro Controller`（VID `0x057E`, PID `0x2009`），且屏幕顶部状态徽章从 `NO-USB` 变为 `ATTACH` / `HS-80`。
2. 如已安装 Python `hidapi`（`pip install hidapi`），可运行：
   ```bash
   python tools/verify_usb_device.py
   ```
   验证 `80 01~04` 握手、`0x31` 模式下 **362 字节完整长度**（无 64 字节截断）以及 CRC-8 校验全部通过。

### Nintendo Switch 实机测试（核心验收）
1. **前提设置（必开！）**：进入 Switch 主菜单 **`系统设置 -> 手柄与感应器 -> Pro 手柄的有线通信`**，设为 **开启**。
2. 用 **USB-C 转 USB-A 数据线**（插 Switch 底座侧边 USB 口），或 **USB-C OTG 线/双 C 线**（直连掌机底部 USB-C 口）连接 Cardputer-Adv。
3. 观察 Cardputer-Adv 屏幕：
   - 正常握手后顶部状态变为 **`PRO-30`**，且 Switch 左上角弹出“有线手柄已连接”/玩家指示灯亮起。
   - 若需要切为 1P：进入 Switch **`手柄 -> 更改握法/顺序`**，在 Cardputer-Adv 按 `Tab` 切到游戏手柄模式，同时按住 **`Q` + `E`**（`L + R`）即可确认！
4. **测试 Amiibo 读卡**：
   - 在 Switch **`系统设置 -> amiibo -> 登记或变更拥有者 / 删除游戏数据`**，或直接进入支持 Amiibo 的游戏（如《塞尔达传说：旷野之息 / 王国之泪》开启 Amiibo 技能、或《集合啦！动物森友会》刷卡机）。
   - 当 Switch 提示“请轻触 amiibo”时，观察 Cardputer-Adv 顶部状态变为 **`NFC-31`**，右下角状态出现 **`POLL`**。
   - 在浏览器里选定 `.bin` 按 **`Enter`**（或按 **`Space` / `G0`** 放卡），状态变为 **`TAG:ON`** -> **`FOUND`**，屏幕底部 `R:1` 读取计数加一，游戏内立刻掉出道具/弹出 Amiibo 识别界面！
   - 测试无限刷卡：按 **`R`**（或开启 **`U` `AUTO-RND`**），Switch 会将该次识别为同角色新卡片并再次发奖。
