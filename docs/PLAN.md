# C1Xiaozhi 移植方案：在快易典 C1 Slim（MIPS Linux）上运行官方小智

> 状态：方案阶段，**尚未开始编码**。本文是给后续执行者（人或模型）的施工蓝图。
> 日期：2026-09-16。上游参考版本：`78/xiaozhi-esp32` @ `5d54beb7`（2026-09-16）。
> 配套文档：[RESEARCH.md](RESEARCH.md)（同类项目调研）、[DEVICE_PROBE.md](DEVICE_PROBE.md)（真机探测清单）。

---

## 0. 一句话结论

**用 C++17/23 把 `xiaozhi-esp32` 的可移植核心（协议、状态机、MCP、OTA/激活、音频服务、显示接口）原样搬到 Linux，
补一层 C1 Slim 专属的「板级」实现（tinyalsa 音频、1bpp 墨水屏显示、evdev 键盘、JSON 设置文件、mbedtls 网络），
编译成一个静态链接的 MIPS32r2 o32 hard-float ELF，命名 `c1xiaozhi`，接入 C1Home / C1ancher 启动器。**

不选多进程架构（100ask 的 3 进程 + UDP IPC），不选 Python sidecar（CardputerZero 方案），不选 Rust/Go 核心（工具链或 Opus 生态不成熟）。
理由见 §3 与 [RESEARCH.md](RESEARCH.md)。

---

## 1. 目标硬件事实（已确认 / 待确认）

来源：`C1-Slim-Ports/docs/*.md`、`C1auncher/docs/*.md`、`C1auncher/Pinao/*.go`、`C1auncher/App/music-player/*.go`。

| 项目 | 值 | 状态 |
| --- | --- | --- |
| SoC | 君正 Ingenic X1600 / X1600E（Halley6，`ingenic,halley6_v20`），XBurst1 @ 1.0 GHz 单核 + XBurst0 协处理器，MIPS32r2，小端，**硬浮点** | 已确认（内核 DT + 官方规格） |
| Cache | 16 KB I / 16 KB D / 128 KB L2 | 官方规格 |
| 内存 | 约 **50 MiB 可用**（X1600E 封装内 64 MB LPDDR2；内核+原厂进程占一部分） | 已确认约 50 MiB |
| 内核 | Linux 5.10.186 MIPS，`PREEMPT`，`CONFIG_MODULES=y`、无模块签名，`/dev/mem` 可用，无 KASLR | 已确认 |
| 存储 | eMMC ~58 GB：p5 `/usr/resource` 200 MB、p6 `/usr/data` 100 MB（可写，放启动桥/小配置）、p8 `/storage` 57 GB（可写，放大文件）；根分区 ext4 **只读** | 已确认 |
| 屏幕 | 296×152，1bpp 纯黑白，控制器 SSD1680；`/dev/epaper_lcd` 整帧 `write()` 5624 字节；sysfs `/sys/devices/platform/e0266a128/epaper/{fast_refresh_only,refresh,refresh_max,refresh_cnt}`；快刷 ~685 ms/帧，全刷 ~1690 ms | 已确认（EPAPER_REFRESH.md） |
| 帧格式 | `offset = (y>>3)*296 + x`，`mask = 0x80 >> (y&7)`，黑 = 1 | 已确认 |
| 键盘 | `/dev/input/event0`（matrix keypad，字母区）+ `/dev/input/event1`（gpio keys：电源 KEY_POWER=116 / KEY_WAKEUP=143、Home、音量等）；MIPS 上 `EVIOCGRAB = 0x80044590` | 已确认 |
| 音频输出 | ALSA card 0，`aplay -D hw:0,0 -f S16_LE -r 48000 -c 2` 已被 Pinao 验证；混音器 `numid=1,iface=MIXER,name=DAC Playback Volume`，范围 0–158；设备自带 `/usr/bin/aplay`、`amixer`、`/usr/sbin/alsactl`、`/usr/bin/ffplay` | 已确认 |
| **音频输入（麦克风）** | 未知。X1600 内置 codec 有 ADC，MagicPen 系列（词典笔）大概率有 MIC，但 C1 Slim 是否焊了 MIC、ALSA 是否暴露 capture 设备、支持哪些采样率 **必须真机验证** | **待确认（阻塞项）** |
| Wi-Fi | `wlan0`，`wpa_supplicant -D nl80211` + `udhcpc`，原厂脚本 `/bin/wifi_up.sh`；C1ancher 已实现完整 Wi-Fi 服务（`C1ancher/src/services/wifi.c`） | 已确认 |
| TLS 根证书 | 设备原厂 HTTPS 客户端**不校验证书**（root ADB 方案就是利用这点），因此 `/etc/ssl/certs` 很可能为空或不可信 | 待确认；方案默认**内嵌 CA bundle** |
| 电池 | `/sys/class/power_supply/*`（C1ancher 有读取实现） | 待确认路径 |
| 启动器约定 | 独占锁 `/dev/shm/c1ancher-external-app.lock`（flock）；启动时 `SIGSTOP` 原厂 `mpenMain`、退出 `SIGCONT`；退出前恢复 `fast_refresh_only`、`refresh_max`、alsactl 状态、等待按键释放；长按 Home 2 s 回 C1Home | 已确认 |
| 应用形态 | Linux ELF32 MIPS 小端 o32 MIPS32r2 双精度硬浮点 **静态链接**，无沙箱，root 运行 | 已确认 |

**阻塞项只有一个：麦克风/采集。** 若无 MIC，仍可做「键盘文字对话 + TTS 播放」版本（见 §7 备选），但那不是"完整功能"。

---

## 2. 上游（xiaozhi-esp32）需要对齐的功能清单

从 `xiaozhi-esp32/README.md`、`docs/websocket_zh.md`、`docs/mqtt-udp_zh.md`、`docs/mcp-protocol_zh.md`、`main/*` 提炼。**✅ = 本方案纳入；🔶 = 后期/可选；❌ = 硬件无关或不适用。**

| 功能 | 上游实现 | 本方案 |
| --- | --- | --- |
| OTA 检查 + 设备激活（激活码 / challenge-HMAC） | `main/ota.cc`，POST `https://api.tenclass.net/xiaozhi/ota/`，头 `Device-Id`(MAC)、`Client-Id`(UUID)、`Activation-Version` | ✅ Activation-Version 1（无 eFuse 序列号，不做 HMAC 激活）；激活码显示在墨水屏 |
| WebSocket 传输（协议版本 1/2/3） | `main/protocols/websocket_protocol.cc` | ✅ v1 默认，v2（时间戳，服务端 AEC）与 v3 一并实现 |
| MQTT + UDP（AES-CTR 加密音频） | `main/protocols/mqtt_protocol.cc` | ✅ 第 3 阶段 |
| hello / listen / abort / detect / stt / tts / llm / mcp / system / custom / alert 消息 | `main/application.cc` | ✅ 全部 |
| Opus 16 kHz 单声道 60 ms 上行；下行 24 kHz（或服务端指定），设备侧重采样 | `main/audio/audio_service.cc` | ✅ 解码器直接以硬件采样率（48 kHz）创建，Opus 内建重采样，**不需要 speex resampler**；上行若 ADC 不支持 16 kHz 则 48k→16k 3:1 抽取 |
| 设备状态机（Starting/WifiConfiguring/Activating/Idle/Connecting/Listening/Speaking/Notifying/Upgrading/AudioTesting/FatalError） | `main/device_state_machine.cc` | ✅ 原样移植 |
| 监听模式 auto / manual / realtime | `Protocol::SendStartListening` | ✅ auto + manual；realtime 需 AEC，🔶（可选走服务端 AEC v2） |
| 设备侧 MCP（`self.get_device_status`、`self.audio_speaker.set_volume`、`self.screen.set_theme`…） | `main/mcp_server.cc` | ✅ 原样移植 + C1 专属工具 |
| 显示：状态栏、表情、聊天字幕、通知、Alert | `main/display/*` (LVGL) | ✅ 重写为 1bpp 位图字体渲染器（复用 C1Home `c1gfx.h` + `C1FONT1` 字体） |
| Glyph Push（服务端推送字形） | `main/protocols/text_glyph_payload.cc` | 🔶 本地已有完整 CJK 位图字体，hello 中先声明 `glyph_push:false` |
| 提示音（ogg/opus：success、exclamation、popup、low_battery、vibration、多语言 activation/upgrade 等） | `main/assets/*.ogg` + `main/audio/demuxer/ogg_demuxer.cc` | ✅ 移植 ogg 解复用器，直接播放上游 ogg |
| 39 种界面语言 | `main/assets/locales/*` → `lang_config.h` | ✅ 复用生成脚本，先做 zh-CN / en-US |
| 离线唤醒词（ESP-SR WakeNet/MultiNet） | Xtensa/RISC-V 专用，不可移植 | 🔶 第 4 阶段：microWakeWord (TFLite-Micro C++) 实验；第 1–3 阶段用**按键 push-to-talk** |
| 设备侧 AEC | ESP-SR AFE | ❌ 无；半双工（等同 ESP32-C3 lite 路径） |
| 声纹识别 3D-Speaker | 服务端功能 | ✅ 无需设备工作（随 detect 上传唤醒音频） |
| 摄像头视觉 | 板级可选 | ❌ |
| Wi-Fi 配网（热点 / BluFi） | `esp-wifi-connect` | 🔶 复用 C1ancher/原厂 Wi-Fi；应用内只显示状态 |
| 电池显示、电源管理（power_save_timer / sleep_timer） | `boards/common/*` | ✅ 读 sysfs；空闲关闭 ALSA 设备、降低刷新 |
| 固件 OTA 下载升级 | `esp_ota` | 🔶 自实现：下载 ELF 到 `/storage`，sha256 校验，原子替换，重新 exec |
| 自定义资源包（assets） | `esp_mmap_assets` | 🔶 |
| 4G / 以太网 / RNDIS | 板级 | ❌ |

---

## 3. 架构

```
┌──────────────────────────── c1xiaozhi (单进程, 静态 ELF) ────────────────────────────┐
│  main.cc ── Application (移植 application.cc: 事件循环 / 状态机 / 消息分发)             │
│     ├─ Protocol ─┬─ WebsocketProtocol ── WebSocketClient ── TlsSocket(mbedtls)         │
│     │            └─ MqttProtocol ── MqttClient(MQTT-C) + UdpSocket + AES-CTR(mbedtls)   │
│     ├─ Ota ── HttpClient(自研, HTTP/1.1 + chunked, over TlsSocket)                     │
│     ├─ McpServer (移植) ── tools: self.* + c1.*                                          │
│     ├─ AudioService (移植) ── OpusEncoder/Decoder(libopus) ── AudioCodec               │
│     │                                  └─ TinyAlsaCodec (pcm_open hw:0,0 / capture)   │
│     ├─ Display ── EpaperDisplay (1bpp framebuffer, C1FONT1 fonts, /dev/epaper_lcd)    │
│     ├─ Board ── C1SlimBoard: Network(wlan0 状态) / Battery(sysfs) / Buttons(evdev)     │
│     └─ Settings ── JSON 文件 (/storage/c1/xiaozhi/settings.json, 命名空间同 NVS)       │
│  线程：main-loop, audio-input, audio-output, opus-codec, network-rx, display-flush, input │
└──────────────────────────────────────────────────────────────────────────────────────┘
```

### 3.1 为什么是单进程 C++ 静态二进制

- 50 MiB RAM、单核 1 GHz：多进程 + UDP IPC（100ask 方案）多占内存、多一跳延迟、多一次 PCM 拷贝，收益为零。
- 上游代码本身就是单进程多任务（FreeRTOS task ≈ pthread），逐文件对应移植最省事、最容易跟上游同步。
- C1 平台已有约定：静态链接、无动态库依赖（`readelf` 检查 "There is no dynamic section"）。
- Rust：`mipsel-unknown-linux-musl` 已降为 Tier 3，需要 `-Zbuild-std`，rustls 在该目标上有已知失败（见 RESEARCH）。
- Go：C1 上 Go 生态成熟（Terminal/News/Pinao），但纯 Go 没有可用的 Opus **编码器**（`pion/opus` 只解码），cgo + mipsle 交叉又回到 C 工具链。Go 可作为工具/服务端脚本，不作为核心。

### 3.2 第三方库（全部静态、全部 vendored）

| 库 | 版本 | 用途 | 说明 |
| --- | --- | --- | --- |
| libopus | 1.5.2 | 编解码 | `--enable-fixed-point --disable-extra-programs --disable-doc`；MIPS 有专用定点优化（`silk/mips`, `celt/mips`）。先定点，后对比浮点（X1600 有 FPU，但定点在此类核上通常更快）。编码 complexity 先设 3，实测 CPU 后调整 |
| mbedtls | 3.6.x LTS | TLS 客户端、SHA-1（WS accept）、SHA-256/HMAC、AES-CTR（UDP 音频）、base64 | 裁剪 config：TLS 1.2+1.3 client only，ECDHE/RSA，AES-GCM/ChaCha20，关闭 server side / DTLS / 大部分老算法 |
| CA bundle | Mozilla `cacert.pem` 当前版 | 验证 xiaozhi.me / tenclass.net | 编译期嵌入（~230 KB）；提供 `--insecure` 开关便于调试自建服务器 |
| cJSON | 1.7.18 | JSON | 与上游一致，最大化复用代码 |
| tinyalsa | 2.0.0 | PCM 采集/播放 + mixer | 直接走 `/dev/snd/pcmC0D0{p,c}` ioctl；比 libasound 小两个数量级。备选：`popen("/usr/bin/aplay ...")` 管道（Pinao 已验证） |
| MQTT-C | 1.1.6 | MQTT 3.1.1 客户端 | MIT，单 .c 文件，套自研 TlsSocket |
| ogg 解复用 | 上游 `ogg_demuxer.cc` | 提示音 | 直接复制 |
| 字体 | Fusion Pixel 12px（C1Bible/C1Home 已用）、文泉驿 15/16 | 中文/英文/符号 | 沿用 `C1Home/tools/build_font.py` 产出的 `C1FONT1` 格式 |
| （可选）TFLite-Micro + microWakeWord 模型 | — | 唤醒词 | 第 4 阶段 |
| （可选）speexdsp | 1.2.1 | AGC / 降噪 / preprocess | 上行音质不佳时再加 |

**自研（不引第三方）**：WebSocket 客户端（RFC 6455，~400 行：握手、掩码、分片、ping/pong、close）、HTTP/1.1 客户端（~300 行）、TlsSocket 封装、UUID v4、Settings（JSON）、日志。

### 3.3 工具链

两条已在 C1 上验证的路线，任选其一，**推荐 Zig**：

| | Zig | GCC (Docker) |
| --- | --- | --- |
| 命令 | `zig c++ -target mipsel-linux-musleabi -mcpu=mips32r2 -std=c++23 -Oz -static` | `mipsel-linux-gnu-g++ -march=mips32r2 -mabi=32 -mhard-float -mfp32 -static` (Ubuntu 24.04 gcc 13) |
| 已验证项目 | C1Sudoku、C1Bible（C++17）、C1LavaX | C1Home、C1ancher、examples/hello |
| libc | musl 静态，产物更小 | glibc 静态（getaddrinfo 等会有 NSS 警告，但能跑） |
| C++23 `std::expected`（上游 `NetworkResult<>` 用到） | clang 19+/libc++ 支持 | gcc 13 支持 |
| 第三方 C 库 | CMake + `CMAKE_C_COMPILER="zig;cc;-target;..."` 工具链文件 | 常规 CMake toolchain |

验收：`readelf -h/-A` 显示 `ELF32 / MIPS / o32 / mips32r2 / hard-float(FP32)`，`readelf -d` 输出 "There is no dynamic section"，与 `C1Home/build.sh` 产物一致。

### 3.4 内存 / CPU 预算（目标）

| 项 | 预算 |
| --- | --- |
| 进程 RSS（对话中） | **< 12 MiB**（Opus enc+dec ≈ 1 MiB，TLS 收发缓冲 2×16 KiB，音频队列 < 512 KiB，帧缓冲 5.6 KiB×3，字体 mmap） |
| CPU（对话中） | Opus 编码 16k/60ms/complexity 3 ≈ 5–10 %；解码 48k ≈ 5 %；TLS AES ≈ 2 %；总 **< 30 %**，为唤醒词留余量 |
| 二进制体积 | < 3 MiB（含 CA bundle、提示音、两套字体则 < 5 MiB） |
| 端到端延迟 | 松键→首包 TTS 播放 ≤ 上游 ESP32 水平（网络主导） |

---

## 4. 源码复用映射（xiaozhi-esp32 → C1Xiaozhi）

| 上游文件 | 处理 | 备注 |
| --- | --- | --- |
| `main/protocols/protocol.{h,cc}` | **复制** | 去掉 `esp_log` → `c1_log` |
| `main/protocols/websocket_protocol.{h,cc}` | **复制**，`network->CreateWebSocket()` 换成自研 `WebSocketClient` | 保留 v1/v2/v3 二进制协议 |
| `main/protocols/mqtt_protocol.{h,cc}` | **复制**，Mqtt/Udp 换自研；`mbedtls_aes_crypt_ctr` 直接可用 | |
| `main/protocols/text_glyph_payload.*` | 复制，暂不启用 | |
| `main/device_state_machine.*`、`main/device_state.h` | **复制** | |
| `main/mcp_server.{h,cc}` | **复制**，去掉 `esp_app_desc`、camera；`Board::GetInstance()` 保持接口 | 新增 `c1.*` 工具 |
| `main/ota.{h,cc}` | **改写**：HTTP 换自研；去掉 eFuse/HMAC 分支（Activation-Version=1）；固件升级改为 ELF 自更新 | POST body 字段见 §5.2 |
| `main/application.{h,cc}` | **改写**（保留流程与消息处理原文，替换 FreeRTOS 事件位 → `std::condition_variable` + 事件队列，`Schedule()` 语义不变） | 这是最大的一块，约 1400 行 |
| `main/audio/audio_service.{h,cc}`、`fixed_queue.h` | **改写**：`esp_audio_codec` opus 封装 → libopus；task → pthread；`AudioEngine` 只保留 `LiteAudioEngine` 等价物（无 AFE） | |
| `main/audio/demuxer/ogg_demuxer.*` | **复制** | |
| `main/audio/audio_codec.{h,cc}` | 接口保留，新实现 `tinyalsa_audio_codec.cc` | |
| `main/display/display.h` | 接口保留，新实现 `epaper_display.{h,cc}` | LVGL 相关成员删掉 |
| `main/settings.{h,cc}` | **重写**：NVS → JSON 文件，API 签名不变（`Settings("websocket", true).GetString("url")`） | |
| `main/system_info.*` | **重写**：MAC 读 `/sys/class/net/wlan0/address`；heap 读 `/proc/self/statm`；flash 读 eMMC 容量；chip `"x1600"` | |
| `main/boards/common/board.{h,cc}`、`wifi_board.*` | 参考，写 `boards/c1_slim/c1_slim_board.cc`（`DECLARE_BOARD`） | `GetSystemInfoJson()` 字段对齐 |
| `main/boards/common/button.*`、`power_save_timer.*`、`sleep_timer.*` | 改写为 evdev / POSIX timer | |
| `main/assets/locales/*`、`scripts/gen_lang.py` | **复制**生成 `lang_config.h` | |
| `main/assets/common/*.ogg`、`locales/*/*.ogg` | **复制**，编译期嵌入（`xxd -i` 或 `.incbin`） | |
| `main/cjson_utils.h` | 复制 | |
| `main/led/*`、`display/lcd_display.cc`、`oled_display.cc`、`emote_display.cc`、`boards/**`（其余） | 不用 | |

---

## 5. 关键协议事实（执行者必读，来自上游源码）

### 5.1 WebSocket

- 握手头：`Authorization: Bearer <token>`、`Protocol-Version: 1|2|3`、`Device-Id: <wlan0 MAC, 小写冒号分隔>`、`Client-Id: <UUID v4, 持久化>`。
- 设备 hello：`{"type":"hello","version":1,"features":{"mcp":true},"transport":"websocket","audio_params":{"format":"opus","sample_rate":16000,"channels":1,"frame_duration":60}}`；10 s 内等服务端 `{"type":"hello","transport":"websocket","session_id":...,"audio_params":{"sample_rate":24000,...}}`。
- 二进制帧：v1 = 裸 Opus；v2 = `{u16 version,u16 type,u32 reserved,u32 timestamp,u32 payload_size,payload}`（网络字节序）；v3 = `{u8 type,u8 reserved,u16 payload_size,payload}`。
- 文本帧类型：`listen`(start/stop/detect + mode auto/manual/realtime)、`abort`(reason)、`mcp`、`stt`、`llm`(emotion)、`tts`(start/stop/sentence_start)、`system`(reboot)、`custom`、`alert`(status/message/emotion)。
- Listening 状态下收到的下行音频丢弃。

### 5.2 OTA / 激活（`https://api.tenclass.net/xiaozhi/ota/`）

- 请求头：`Activation-Version: 1`、`Device-Id`、`Client-Id`、`User-Agent: <board>/<version>`、`Accept-Language: zh-CN`、`Content-Type: application/json`。
- POST body（`Board::GetSystemInfoJson()`）：`version:2, language, flash_size, minimum_free_heap_size, mac_address, uuid, chip_model_name, chip_info{model,cores,revision,features}, application{name,version,compile_time,idf_version,elf_sha256}, partition_table[], ota{label}, display{monochrome:true,width:296,height:152}, board{type,name,ssid,rssi,channel,ip,mac}`。
- 响应：`activation{message,code,challenge,timeout_ms}`（有则进入 Activating，屏幕显示 code + 播放 activation 提示音）、`websocket{url,token,version}`、`mqtt{endpoint,client_id,username,password,publish_topic,subscribe_topic}`、`server_time{timestamp,timezone_offset}`、`firmware{version,url,force}`。
- 设置持久化命名空间与 NVS 一致：`wifi.ota_url`、`websocket.{url,token,version}`、`mqtt.*`、`board.uuid`、`assets.download_url`。

### 5.3 MCP

- 外层 `{"session_id","type":"mcp","payload":{JSON-RPC 2.0}}`；方法 `initialize`（返回 protocolVersion `2024-11-05`、serverInfo）、`tools/list`（分页 cursor）、`tools/call`；返回 `{"content":[{"type":"text","text":"..."}],"isError":false}`。
- 上游内置工具：`self.get_device_status`、`self.audio_speaker.set_volume`、`self.screen.set_brightness`（无背光，省略）、`self.screen.set_theme`（映射为反色）、`self.camera.take_photo`（省略）。
- C1 新增建议：`c1.screen.show_text`、`c1.system.reboot`、`c1.launcher.open_app`（受白名单约束）、`c1.keyboard.type_chat`（见 §7）。

### 5.4 MQTT + UDP

- MQTT hello `transport:"udp"`，响应含 `udp{server,port,key,nonce}`；音频 UDP 包 = 16 字节 nonce 头（含 payload 长度、序号）+ AES-128-CTR 密文；序号严格递增，防重放。细节以 `mqtt_protocol.cc` 为准。

---

## 6. 分阶段施工计划与验收标准

### Phase 0 — 真机探测（0.5 天，阻塞后续）

按 [DEVICE_PROBE.md](DEVICE_PROBE.md) 执行只读命令，把结果回填到该文档。关键产出：
1. `arecord -l` / `/proc/asound/pcm` 是否有 capture；`arecord -D hw:0,0 -f S16_LE -r 16000 -c 1 -d 3 /tmp/t.wav` 能否录到人声（adb pull 回来听）。
2. 可用采样率/声道（`/proc/asound/card0/pcm0c/sub0/hw_params` 或 `arecord --dump-hw-params`）。
3. `free`、`/proc/cpuinfo`、`cat /sys/devices/system/cpu/cpu0/cpufreq/*`（是否有调频）。
4. `/etc/ssl` 是否有 CA；`date` 是否准确（TLS 证书有效期校验依赖时间！设备无 RTC 电池时需 NTP/`server_time` 校时）。
5. `/sys/class/power_supply/`、`/sys/class/net/wlan0/address`、`evtest`/`hexdump /dev/input/event*` 按键码表。

**通过标准**：录到清晰人声 + 播放正常。否则转 §7 备选路线。

### Phase 1 — 工具链与依赖（1–2 天）

- `third_party/` 加子模块：opus、mbedtls、cJSON、tinyalsa、MQTT-C；`cmake/toolchain-zig-mipsel.cmake` 与 `cmake/toolchain-gcc-mipsel.cmake`。
- `tools/build.sh`：一键产出 `build/c1xiaozhi` + ELF 校验（架构/ABI/静态）。
- 样例 `tools/probe/audio_loop`：tinyalsa 录 3 s → Opus 编码 → 解码 → 播放；打印每帧编码耗时与进程 RSS。
- **验收**：真机 `audio_loop` 回放清晰；16k/60ms 编码单帧 < 6 ms（≈10 % CPU）。QEMU-user（`qemu-mipsel`）能跑单元测试。

### Phase 2 — MVP 对话（3–5 天）

- 自研 `TlsSocket` / `HttpClient` / `WebSocketClient`；`Settings`；`SystemInfo`；`Ota::CheckVersion` + 激活码墨水屏显示。
- 移植 `Protocol` / `WebsocketProtocol` / `DeviceStateMachine` / `AudioService`（仅 lite 路径）/ `Application` 主流程。
- `EpaperDisplay` 最小版：状态栏（Wi-Fi / 电量 / 状态文字）、聊天字幕区（自动换行、滚动）、通知条。
- 输入：Enter/OK 按住说话（manual），单击切换 auto 监听；Back/Esc 中止 TTS；音量键；长按 Home 退出。
- 启动器集成：app lease、SIGSTOP/SIGCONT、退出恢复屏幕/音频状态（照抄 Pinao/C1Home 做法）。
- **验收**：在 xiaozhi.me 绑定设备后，按键说"今天天气"→ 屏幕出现 STT 文本 → 听到 TTS → 屏幕出现回答字幕；连续 30 轮不崩、RSS < 12 MiB。

### Phase 3 — 功能对齐（3–5 天）

- `McpServer` + 工具；`MqttProtocol` + UDP；提示音（ogg）；`llm` 表情 → 单色表情位图（≥ 20 种 emoji 的 24×24 位图）；`alert`/`system reboot`/`custom`；多语言；电池/低电提示；power save（空闲 N 分钟关 ALSA、停止刷新；再按键唤醒）。
- 刷新策略：`fast_refresh_only=1`；帧去重；字幕逐句更新（`tts.sentence_start`）而非逐字；状态切换时全刷一次去残影；≥ 700 ms 节流。
- 设置页（应用内）：服务器 OTA URL 覆盖、协议版本、音量、自动监听、语言；写 `settings.json`。
- **验收**：对照 §2 表中 ✅ 项逐条打勾；MQTT+UDP 与 WebSocket 均能完整对话；MCP `tools/list` 在 xiaozhi.me 控制台可见并可调用。

### Phase 4 — 增强（按需）

- 唤醒词：TFLite-Micro + microWakeWord（"你好小智" 需自训；先用现成英文模型验证 CPU 占用）。
- 键盘文字对话（§7）。
- 自更新：GitHub Release JSON → 下载 → sha256 → 原子替换 → `execv` 自身。
- Glyph Push、自定义资源包、服务端 AEC（v2 时间戳）+ realtime 模式。
- 发布：打包为 C1ancher 应用包（`c1pkg`）/ C1Home `apps.conf` 条目；提交到 C1ancher 应用仓库。

---

## 7. 备选 / 降级路线

- **无麦克风或无 capture 设备**：
  1. 键盘文字对话——发送 `{"type":"listen","state":"detect","text":"<用户输入>"}`。这是社区服务端（xiaozhi-esp32-server 等）把 detect 文本当作用户输入的惯用法；官方 xiaozhi.me 是否接受需实测。TTS 照常播放。
  2. 外接 USB 声卡（X1600 有 USB OTG）：内核是否带 `snd-usb-audio` 需查 `/proc/config.gz` 或 `/lib/modules`；内核可加载模块（无签名），可自编译 5.10.186 模块。
- **libopus 定点性能不足**：换浮点构建（有 FPU）；或把上行帧长改 100/120 ms 降低调用开销（协议允许 `frame_duration` 协商）。
- **TLS 太慢**：优先 ChaCha20-Poly1305 套件；X1600 有 AES 硬件但内核未必暴露 `AF_ALG`，不指望。
- **静态 musl 的 DNS**：musl 不读 `nsswitch`，只读 `/etc/resolv.conf`（udhcpc 会写），够用；备用内置 DoH 不做。

---

## 8. 仓库布局（建议）

```
C1Xiaozhi/
├── README.md
├── docs/                 PLAN.md / RESEARCH.md / DEVICE_PROBE.md / PROTOCOL_NOTES.md
├── src/
│   ├── main.cc
│   ├── application.{h,cc}          ← 移植
│   ├── device_state_machine.*      ← 复制
│   ├── settings.*  system_info.*   ← 重写
│   ├── protocols/  (protocol, websocket_protocol, mqtt_protocol, text_glyph_payload)
│   ├── net/        (tls_socket, http_client, websocket_client, mqtt_client, udp_socket)
│   ├── audio/      (audio_service, opus_codec, tinyalsa_audio_codec, ogg_demuxer, fixed_queue.h)
│   ├── display/    (display.h, epaper_display, gfx1bpp, fonts, emoji_mono)
│   ├── boards/c1_slim/ (c1_slim_board, buttons_evdev, battery_sysfs, wifi_status, launcher_lease)
│   ├── boards/linux_host/ (开发机模拟：ALSA/CoreAudio 或 WAV 文件 + PNG 帧输出)
│   ├── mcp_server.*  ota.*
│   └── platform/   (log, thread, timer, event_loop)
├── assets/           ogg 提示音、字体 bin、emoji 位图、locales
├── third_party/      opus mbedtls cjson tinyalsa mqtt-c (git submodule)
├── cmake/            toolchain 文件
├── tools/            build.sh、deploy.sh(adb push)、probe/、gen_lang.py、build_font.py
├── device/           启动脚本、apps.conf 片段、c1pkg 清单
└── tests/            host 单元测试（协议解析、状态机、ws 帧编解码）+ qemu-mipsel 冒烟
```

---

## 9. 风险清单

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| 无麦克风 / 无 capture | 核心功能不可用 | Phase 0 先验；§7 降级 |
| 时钟不准导致 TLS 证书校验失败 | 无法联网 | 启动时 SNTP（或用 OTA 响应 `server_time`，首连允许 `--insecure` 拿时间再切回严格校验） |
| 墨水屏 685 ms/帧 | 字幕更新慢、残影 | 逐句更新、节流、状态切换全刷 |
| 单核抢占：音频线程被显示写帧/TLS 阻塞 | 爆音/丢帧 | 音频线程 `SCHED_FIFO` 或高 nice；ALSA 缓冲 ≥ 200 ms；显示写帧走独立线程 |
| 50 MiB 内存与原厂 `mpenMain`（LVGL）共存 | OOM | `SIGSTOP` 不释放内存；必要时由 C1Home 直接启动而不加载原厂 UI |
| 上游快速迭代（171 变体、IDF 6） | 移植分叉 | 记录上游 commit；`protocols/`、`mcp_server`、`state machine` 尽量零改动，便于 diff 同步 |
| 许可 | 开源合规 | 上游 MIT；本仓库建议 MIT（便于回流上游）；opus BSD、mbedtls Apache-2.0、cJSON MIT、tinyalsa BSD、MQTT-C MIT；字体各自许可（Fusion Pixel OFL/MIT、文泉驿 GPL+例外）；C1Home 的 `c1gfx.h` 来自 GPL-3.0 仓库，**复用需改写或同意 GPL** |

---

## 10. 给执行模型的操作提示

1. 先读本文 §1、§5、§6，再读 `xiaozhi-esp32/AGENTS.md` 和 `main/application.cc`。
2. 不要一开始就写 UI；先让 `tools/probe/audio_loop` 和 `tools/probe/ws_echo`（连 xiaozhi.me 完成 hello 握手）在真机跑通。
3. 每个阶段结束用 `readelf` 验证 ELF 属性，用 `adb push /storage/c1/xiaozhi/` + `adb shell` 手工启动测试，不改系统分区。
4. 所有对设备的写操作限定在 `/storage/c1/xiaozhi/` 与 `/usr/data/c1/xiaozhi/`。
5. 报告时区分「编译通过」「QEMU 通过」「真机通过」。
