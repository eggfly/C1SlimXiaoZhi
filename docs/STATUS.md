# 实现状态：做了什么，验证到什么程度

> 更新于 2026-09-16。**代码从未在 C1 Slim 真机上运行过**，设备当时不在手边。
> 下面严格区分「真机验证」「本地端到端验证」「主机单元测试」「仅编译」四种状态。

## 0. 一句话

完整移植已经写完，能编译出正确的 MIPS 静态 ELF，46 项主机单元测试全过，
并且在主机上**与本地测试服务器跑通了完整一轮对话**（OTA → WebSocket 握手 →
hello 协商 → STT → TTS → Opus 上行 → 干净退出）。
但**没有一行代码在 C1 Slim 硬件上跑过**，真机第一步见 §4。

### 本地端到端验证做到哪一步

`tools/test_server.py` 是一个独立实现的最小小智服务器（WebSocket 部分照 RFC 6455
重写，不复用客户端代码，两边互为校验）。实测通过的链路：

| 环节 | 观察到的结果 |
| --- | --- |
| OTA POST | 头部 `Device-Id` / `Client-Id` / `Activation-Version: 1` 正确，body 含完整系统信息与 `display{monochrome,296,152}` |
| 配置下发 | 客户端接受 websocket 端点并持久化 |
| WebSocket 握手 | 升级成功，`Sec-WebSocket-Accept` 摘要双向校验通过 |
| 设备 hello | `{"version":1,"features":{"mcp":true,"glyph_push":false},"audio_params":{"opus",16000,1,60}}` |
| 服务器 hello | session 被记录，下行采样率 24000 被接受 |
| listen | `{"state":"start","mode":"auto"}` |
| STT | 中文 `测试一下` 正确解析并显示 |
| TTS | start → sentence_start → stop，状态机 listening → speaking → listening |
| 半双工 | speaking 期间采集自动关闭，stop 后自动恢复 |
| Opus 上行 | 连续 125+ 帧；静音段是 1 字节 DTX 帧，说明 DTX 生效 |
| 退出 | SIGTERM 后干净关闭，服务器侧看到正常断开 |

复现：

```bash
python3 tools/test_server.py --port 8099 &
(sleep 2; echo t; sleep 10; echo q) | build/host/c1xiaozhi --ota-url http://127.0.0.1:8099/ota/ -v
```

## 1. 产物

| 项 | 值 |
| --- | --- |
| 二进制 | `build/device/c1xiaozhi`，1878588 字节 |
| 架构 | ELF32 MIPS 小端，`0x70001005`（noreorder, cpic, o32, mips32r2） |
| 浮点 | 硬浮点双精度，CPR1 = 32 位 |
| 链接 | 静态，无动态段 |
| 对照 | 以上每一项都与原厂 `mpenMain` 完全一致 |
| 字体 | `build/device/font.bin`，22415 字形，876 KiB，运行时 mmap |

体积里有 189 KiB 是内嵌的 CA 根证书。

## 2. 功能对照（相对 xiaozhi-esp32）

| 功能 | 状态 | 验证程度 |
| --- | --- | --- |
| OTA 版本检查与配置下发 | 已实现 | **本地端到端验证通过** |
| 设备激活（验证码 + 轮询，Activation-Version 1） | 已实现 | 仅编译；测试服务器支持 `--activation-code` 可手工验证 |
| 激活 Version 2（序列号 + HMAC-SHA256） | 已实现 | 仅编译；密钥存设置文件，**非硬件保护** |
| 服务器校时 | 已实现 | **本地端到端验证通过** |
| WebSocket 传输 | 已实现 | **本地端到端验证通过**（握手、摘要、掩码、分片、ping/pong） |
| 二进制协议 v1 / v2 / v3 | 已实现 | v1 **本地端到端验证通过**；v2/v3 仅编译 |
| MQTT + UDP 传输 | 已实现 | 仅编译 |
| UDP 音频 AES-128-CTR 加解密 | 已实现 | 仅编译 |
| UDP 序号防重放 | 已实现 | 仅编译 |
| hello / listen / stt / tts / llm | 已实现 | **本地端到端验证通过** |
| abort / mcp / system / alert / goodbye | 已实现 | 仅编译 |
| Opus 16 kHz 单声道 60 ms 上行 | 已实现 | **主机测试通过** |
| 下行解码到 48 kHz（免独立重采样器） | 已实现 | **主机测试通过** |
| 设备状态机 | 逐字移植 | **主机测试通过**（7 项） |
| 设备侧 MCP（initialize / tools/list 分页 / tools/call） | 已实现 | **主机测试通过**（7 项） |
| MCP 工具：设备状态、音量、屏幕消息 | 已实现 | 部分主机测试 |
| 墨水屏渲染与刷新节流 | 已实现 | **字节序主机测试通过**，刷新时序未验证 |
| 中文字体渲染（Unifont） | 已实现 | **主机测试通过**（ASCII 回退、换行、越界） |
| 按键输入（evdev） | 已实现 | 仅编译；**键码映射是猜的**，见 §4 |
| 启动器共存（flock + SIGSTOP） | 已实现 | 仅编译 |
| Wi-Fi 状态（wpa_supplicant 套接字） | 已实现 | 仅编译 |
| 电池（sysfs） | 已实现 | 仅编译；**供电节点名未确认** |
| 提示音（Ogg Opus） | 已实现 | **解析主机测试通过**（6 项） |
| 自更新（下载、校验 ELF、原子替换、re-exec） | 已实现 | 仅编译 |
| 设置持久化 | 已实现 | **主机测试通过**（5 项） |
| 离线唤醒词 | **未实现** | 用按键 push-to-talk 代替 |
| 设备侧 AEC | **未实现** | 硬件有回环参考通道，见 §5 |
| 摄像头视觉 | **未实现** | 不适用 |
| Glyph Push | **未实现** | 本地已有完整 CJK 字体，hello 里声明 false |
| 多语言界面 | **未实现** | 目前仅中文硬编码 |
| Wi-Fi 配网 | **不做** | 沿用系统/启动器的 Wi-Fi |

## 3. 主机测试

`tools/build.sh host test`，46 项全过。覆盖：

- URL 解析 8 项，含 IPv6、userinfo 剥离、非法输入
- 设置持久化 5 项，含损坏文件恢复、只读句柄、旧格式兼容
- 状态机 7 项，含非法跳转拒绝、监听器增删
- 绘图与字体 9 项，其中 **帧字节序按驱动公式逐位核对**
- Opus 5 项，含 16k→48k 往返、丢包隐藏、垃圾输入
- Ogg 解析 6 项，含跨段包、截断、垃圾输入
- MCP 7 项，含分页、缺参拒绝、未知方法

主机上 Opus 编码耗时 0.587 ms/帧。**这是 ARM64 Mac 的数字，不能外推到 1 GHz MIPS**，真机预算必须实测。

## 4. 真机第一步（按顺序）

1. **确认麦克风**。先跑 [DEVICE_PROBE.md](DEVICE_PROBE.md)。整个上行链路建立在
   [MICROPHONE.md](MICROPHONE.md) 的逆向结论上，那些结论本身没有实测过。
2. **抓键码**。`src/boards/c1_slim/input_evdev.cc` 里的映射用的是标准 Linux 键码，
   但这台设备的矩阵键盘实际发什么码**没人确认过**。先 `hexdump /dev/input/event0`
   记下每个键，再用 `C1XZ_KEYMAP="28=talk,1=cancel"` 覆盖，不用重新编译。
3. **确认电池节点**。`ls /sys/class/power_supply/`，代码是扫目录找 `type=Battery`，
   找不到就报告「无电池」而不是瞎猜。
4. **先不接启动器**。用 `adb shell` 直接跑，确认能正常退出并恢复屏幕，再考虑集成。
5. **看日志**。`--log /storage/c1/xiaozhi/run.log -v`。

## 5. 已知的设计取舍

- **半双工**。没有回声消除，说话时关闭采集，否则麦克风会听到喇叭，服务器会把
  设备自己的声音转写成用户输入。硬件上 ALSA card 0 device 1（`i2s-tloop`）提供
  播放回环参考信号，接上 AEC 就能做全双工，但 CPU 预算未知，没进这一版。
- **前 192 ms 静音**。照抄原厂：每次打开采集丢弃前 3072 帧，压 ES8326 的上电瞬态。
  代价是按下说话键到真正采集有 192 ms 延迟。
- **刷新节流 700 ms**。面板物理上一帧就要 685 ms，提交更快只会排队。相同帧直接丢弃。
- **不重启设备**。服务器的 `system.reboot` 指令被记录后忽略。这个程序以 root 跑在
  只读根分区上，没有我们能控制的恢复路径。
- **提示音不内置**。不重新分发上游音频资源；`tools/deploy.sh` 会从相邻的
  `xiaozhi-esp32` 检出里复制。没有也能正常工作。
- **CA 根证书内嵌**。设备根文件系统没有可用信任库，原厂 HTTPS 客户端干脆不校验证书。
  内嵌 121 张 Mozilla 根证书，约 200 KiB 常驻内存。

## 6. 没做的事

- 没有在 C1 Slim 真机上安装或运行过。
- 没有连过真实的 xiaozhi.me 服务器。本地测试服务器验证了协议形状，但没有验证
  真实服务端对这些字段的接受度，也没有跑过真实的语音识别与合成。
- 激活流程只在测试服务器上验证过 202/200 轮询，没有真正绑定过账号。
- 没有测过内存占用。目标是对话中 RSS < 12 MiB，这是估算不是实测。
- 没有测过功耗与续航。
- MQTT+UDP 路径连自测都没有，只有编译通过。
- 没有做启动器集成（C1ancher 应用包或 C1Home 条目）。
