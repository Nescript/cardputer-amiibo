# 《王国之泪》Amiibo 同日重复刷卡调研

核对日期 2026-11-24。目标是在原厂 Switch 2 上只用现有 Cardputer-Adv 和 C2C 数据线，同一天用同款林克领到多次奖励。

## 已有实现

| 项目 | 已公开的证据 | 对 Cardputer 的参考价值 |
| --- | --- | --- |
| Flashiibo Pro | 厂商手册明确列出《王国之泪》和《旷野之息》，支持手动换 UID 以及自动换 UID，并展示再次领到道具的操作流程。手册未标明该流程使用的主机型号。 | 证明厂商针对该游戏提供了同日重复刷功能。需要把独立 NFC 设备贴到手柄上。 [厂商教程](https://www.flashiibo.com/manual/pro/tutorial/03-bypassing-daily-scan-restrictions/) |
| Pixl.js | 开源固件提供手动和自动换 UID。换 UID 前先验证并解密原标签，然后生成新 UID 并用本地密钥重新签名和加密。NFC 场撤掉后触发读事件，自动模式等待约一秒才换 UID。 | 可参考签名和换卡时机。Pixl.js 使用 NFC 模拟。 [功能手册](https://github.com/solosky/pixl.js/blob/main/docs/en/04-Using-Firmware.md) · [签名代码](https://github.com/solosky/pixl.js/blob/5521e315b62afe25289ce656e536202f7df8f89a/fw/application/src/amiibo_helper.c#L120-L169) · [读事件](https://github.com/solosky/pixl.js/blob/5521e315b62afe25289ce656e536202f7df8f89a/fw/application/src/ntag/ntag_emu_v2.c#L239-L250) · [自动切换](https://github.com/solosky/pixl.js/blob/5521e315b62afe25289ce656e536202f7df8f89a/fw/application/src/app/amiibo/scene/amiibo_scene_amiibo_detail.c#L139-L151) |
| flipper_amiibo | Flipper Zero 应用提供 New UID 和 Blast。Blast 用用户提供的密钥生成新标签，发射一秒、暂停两秒后重复。作者报告测过 Flipper，公开说明未提供与本项目同配置的《王国之泪》测试。 | 可参考循环放卡和取卡。固定定时器不判断游戏是否已经读完。 [项目说明](https://github.com/Soap-n-Cartridge/flipper_amiibo) · [Blast 源码](https://github.com/Soap-n-Cartridge/flipper_amiibo/blob/08092e330a34ab934d607595a56e91aab0fbd0a8/amiibo_app.c#L1974-L2031) |
| emuiibo | 在 Atmosphère 环境内接管主机的 Amiibo 服务，可以在游戏访问时随机 UUID。README 明确举例《旷野之息》。 | 属于主机自制固件方案，和原厂 Switch 2 的外部有线手柄环境不同。 [项目说明](https://github.com/XorTroll/emuiibo#readme) |
| PicoAmiibo-RP2040 | 普通 RP2040 Pico 通过 USB 模拟 Amiibo，项目记录《王国之泪》刷出 Epona。该记录验证一次成功，未记录同日连续扫描同角色。 | 本项目已使用同类有线读取链路，可保留它来验证换 UID 后的标签。 [实机记录](https://github.com/LockingFR/PicoAmiibo-RP2040/blob/main/USER_TEST.md#console-recognition-and-scan) |
| PicoSwitch2 | 发布页报告虚拟标签的载入、呈现、写入和再次呈现经过实机验证。伴侣应用的本地初始化可以验证 HMAC 并重新加密，但用途是重置标签用户数据。公开资料未验证《王国之泪》同日无限刷。 | 能参考 USB 读卡时序及本地密钥管理。 [发布记录](https://github.com/notsosaelin/PicoSwitch2/releases) · [初始化代码](https://github.com/notsosaelin/PicoSwitch2/blob/34fc339c108406468943162deb9c1985b7e0e094/companions/android/app/src/main/java/dev/picoswitch/companion/data/AmiiboCrypto.kt#L138-L163) |

## 适用于当前硬件的验证顺序

1. 先用已有的本地密钥在电脑上验证一份原始林克标签，再生成一份款式相同、UID 不同的 540 字节标签。完成后重新读取并验证两个 HMAC 和角色编号，保留原文件。开源的 amiitool 可以负责验证和重新签名。其源码采用 MIT 许可。 [amiitool 源码](https://github.com/socram8888/amiitool/blob/master/amiibo.c) · [许可](https://github.com/socram8888/amiitool)
2. 用现有 USB 导入方式把两份标签放进 microSD。保持 Switch 2 账户和日期相同，用现有横屏固件在《王国之泪》里分别扫描两份标签，观察是否各发一次奖励。任天堂列出的正常规则是每张 Amiibo 每天一次。 [任天堂说明](https://en-americas-support.nintendo.com/app/answers/detail/a_id/61937)
3. 两次都发奖后再考虑固件内自动签名。在 Core 1 生成下一份标签，等当前 USB 读卡完成并结束扫描后再让下一份标签就绪。当前 `R:` 每返回一个 `0x15` 数据块就加一，借用该计数直接换卡可能中断读取。 [当前计数逻辑](../src/ns2/device.c) · [当前自动换 UID 逻辑](../src/main.cpp) · [扫描结束逻辑](../src/ns2/ns2_virtual_nfc_runtime.c)

现有 `R` 和 `AUTO-RND` 只调整 UID 和 BCC。本地合成标签对照试验中，这组修改通过了 BCC 检查，却无法通过 UID 关联的 HMAC 验证。重新签名后的合成标签通过 HMAC 验证。Cardputer 上的同日二次掉落仍需实机确认。
