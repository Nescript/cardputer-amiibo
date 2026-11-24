# 原厂 Switch 2 上复用 Amiibo 文件的路径

调查截至 2026-11-24。目标设备为原厂 Switch 2，游戏为《王国之泪》。本项目已有 54 个 540 字节 `.bin` 文件，已导入 Cardputer-Adv 的 microSD 卡。[本地数据包说明](ZELDA_AMIIBO_PACK.md)

## 结论

Switch 2 原厂主机上的 USB Pro Controller 2 虚拟 Amiibo 路径已有实机证据。最直接的证据来自 [PicoAmiibo-RP2040](https://github.com/LockingFR/PicoAmiibo-RP2040)，作者报告使用固件 0.1.1 在 Switch 2 的 TOTK 获得 Epona。此实现把 Pro Controller 2 的 USB 协议用于 Amiibo 模拟，硬件没有 NFC。Windows 管理器可导入原始 540 或 572 字节 NTAG215 `.bin`，再写入 Pico。该记录只证明一次具体游戏结果，管理器读回字节相同也不等同于游戏认可。固件 0.1.2 仍待主机实测，管理器 1.1.1 没有新的端到端主机测试，见[项目说明](https://github.com/LockingFR/PicoAmiibo-RP2040#readme)、[实机测试记录](https://github.com/LockingFR/PicoAmiibo-RP2040/blob/main/USER_TEST.md)和[发布说明](https://github.com/LockingFR/PicoAmiibo-RP2040/releases)。

PicoAmiibo 公开固件限定 `PICO_BOARD=pico`，并链接 Pico SDK、TinyUSB 与 Pico Flash 库。项目在 Pico 上把标签写入板载 Flash，Cardputer 移植可以从现有 microSD 读取标签。[CMake 配置](https://github.com/LockingFR/PicoAmiibo-RP2040/blob/main/CMakeLists.txt)，[管理器使用说明](https://github.com/LockingFR/PicoAmiibo-RP2040/blob/main/MANAGER_USAGE.md)

另一个技术参照 [PicoSwitch2](https://github.com/notsosaelin/PicoSwitch2) 使用 Pico W 或 Pico 2 W。其状态页记录 Switch 2 实机已确认 540/572 字节虚拟 Amiibo 读取、游戏写入完成、移卡与再次扫描。适配器本身仍是额外硬件。其状态页也将 Windows Amiibo 传输的实机验证列为待办，Android Controller Link 对真实 Switch 2 的验证同样待办。[兼容矩阵](https://github.com/notsosaelin/PicoSwitch2/blob/main/docs/status/compatibility-matrix.md)和[当前状态](https://github.com/notsosaelin/PicoSwitch2/blob/main/STATUS.md)

## Cardputer 单机判断

本仓库当前固件使用一代 Pro 的 USB 身份 `057E:2009`，并通过 HID `0x11`、`0x31` 报文实现 MCU/NFC 交互。用户在 Switch 2 上已能用它输入按键，游戏进入 Amiibo 扫描时报 `2115-0064`。[本仓库协议说明](PROTOCOL_SPEC.md)

PicoAmiibo 用二代 Pro 身份 `057E:2069`，将手柄输入和厂商 Bulk 命令放在不同接口。Espressif 文档确认 ESP32-S3 支持 HID、厂商自定义类与复合设备。本项目的 Arduino SDK 已启用 TinyUSB HID 和 Vendor，Bulk FIFO 为 64 字节，Pico 参考实现设为 128 字节。移植需验证较小 FIFO 下的收发分片与流控。[PicoAmiibo 运行代码](https://github.com/LockingFR/PicoAmiibo-RP2040/blob/main/src/main.c)，[PicoAmiibo TinyUSB 配置](https://github.com/LockingFR/PicoAmiibo-RP2040/blob/main/include/tusb_config.h)，[ESP32-S3 USB Device 文档](https://docs.espressif.com/projects/esp-usb/en/latest/esp32s3/usb_device.html)

按 PicoAmiibo 源码的长度与 BCC 规则复核了本地 54 个文件，均符合其 540 字节输入要求。建议先实现二代 Pro 的 USB 枚举、握手和 NFC 读命令，沿用现有 microSD 浏览和 RAM 预载，用 `01_TotK/01_Link_TotK.bin` 在《王国之泪》验证一次实际掉落。[PicoAmiibo 标签校验源码](https://github.com/LockingFR/PicoAmiibo-RP2040/blob/main/src/vendor/virtual_amiibo.c)，[实机测试流程](https://github.com/LockingFR/PicoAmiibo-RP2040/blob/main/USER_TEST.md)

无线方向已有 ESP32-S3 上的二代 Pro BLE 手柄实测。`espp` 文档覆盖配对、按键输入、重连与唤醒，该方案依赖支持 5 ms BLE 连接间隔的 ESP-IDF 版本。本次未找到该组件在 Switch 2 上模拟 Amiibo 的实测记录。[espp Switch 2 Pro BLE 文档](https://esp-cpp.github.io/espp/ble/switch2_pro.html)

## 已有设备桥接

JoyCon Droid 当前文档宣称支持 Switch 1、Switch 2，并可通过蓝牙读取 `.bin` Amiibo 文件。其兼容说明要求 Switch 系统版本 12.0.0 及以上的 Android 设备具备 root 配对措施，Amiibo 还要求 Samsung 或具有 `HID_DEV_MTU_SIZE 512` 的自定义 ROM。文档没有给出 Switch 2 与 TOTK 的端到端实测。因此它只适用于用户已经拥有且已满足要求的 Android 设备，本调查不假设设备已 root，也不建议为此新增 root 或刷机操作。[JoyCon Droid 项目说明](https://github.com/TeamJCD/JoyConDroidDocs/blob/main/README.md)、[FAQ](https://github.com/TeamJCD/JoyConDroidDocs/blob/main/faq.md)

Linux `joycontrol` 主线的 README 标记 NFC 数据功能已移除，相关 NFC 实现仍见开放的 [Issue 82](https://github.com/mart1nro/joycontrol/issues/82)。没有找到它支持 Switch 2 Amiibo 的证据。PicoSwitch2 的 Windows、Linux、Android companion 均围绕 Pico 适配器工作，其 companion 验证状态见[项目状态](https://github.com/notsosaelin/PicoSwitch2/blob/main/STATUS.md)。

## 已改机主机与第一代 Switch 对照

emuiibo 的安装文档依赖 Atmosphere、Tesla Overlay 与 Switch SD 卡目录，现有文档未给出原厂 Switch 2 支持依据。它不适用于当前原厂 Switch 2 目标。emuiibo 文档明确列出 raw `.bin` dump 会在主机启动时自动转成新版虚拟 Amiibo 格式；若 `sd:/switch/key_retail.bin` 存在，程序还会提取加密区中的 Mii、名称和游戏区域数据。原 dump 会保留并移入新版目录。新建或编辑虚拟标签的 PC 工具为 emuiigen。[emuiibo 安装、格式与转换说明](https://github.com/XorTroll/emuiibo)

若问题回到第一代 Switch，Nintendo 官方说明开启 Pro Controller Wired Communication 时，Pro Controller 的 NFC 功能不可用，需拔掉 USB 线并启用无线通信。此说明适用于第一代主机的有线 Pro Controller 设置，不能据此推断 Switch 2 的 Pro Controller 2 USB 行为。[Nintendo 支持说明](https://www.nintendo.com/my/support/qa/detail/34532)

## 证据边界

Switch 2 USB 方案的协议和游戏结果来自项目源码、项目维护的兼容记录与作者报告，暂未找到 Nintendo 发布的 Pro Controller 2 Amiibo USB 协议规范。Cardputer 移植尚未编译、烧录或实机验证。JoyCon Droid 的文档虽明确列出 Switch 2 支持，仍缺少本任务所需的 Switch 2 加 TOTK 实测记录。
