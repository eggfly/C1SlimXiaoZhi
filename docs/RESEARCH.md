# 调研：现有的非 Python 小智（xiaozhi）Linux 客户端与 CardputerZero 移植

> 日期：2026-09-16。数据来自 GitHub API / 仓库 README 抓取，星数与时间为抓取时快照。

## 1. 结论摘要

| 项目 | 语言 | 架构 | 协议覆盖 | 唤醒词 | 平台 | 许可 | 对本项目的价值 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| [100askTeam/xiaozhi-linux](https://github.com/100askTeam/xiaozhi-linux) | C/C++ | 3 进程：`sound_app`(ALSA+opus+speexdsp) / `control_center`(websocketpp+Boost+OpenSSL+curl) / `gui`(LVGL)，UDP IPC | WebSocket、hello/listen/tts/stt、激活 | 无 | IMX6ULL、T113、K230、RK、STM32MP157 | 未声明 | 参考 Linux ALSA/opus 用法；**依赖 Boost/OpenSSL/curl 太重，不适合 50 MiB MIPS 静态构建**；2025-07 后无更新 |
| [haoyn231/xiaozhi_linux_rs](https://github.com/haoyn231/xiaozhi_linux_rs) | Rust | 单核心进程 + UDP IPC GUI；tokio、tokio-tungstenite(rustls)、reqwest、alsa、opus、speexdsp | WebSocket、激活、TTS/STT、MCP 网关（subprocess/HTTP/TCP） | 无（明确不做） | x86_64、aarch64、armv7 glibc/uClibc | MIT | 架构参考（MCP 网关设计好）；**Rust 在 mipsel-musl 是 Tier 3，rustls 该目标有已知失败**，不采用 |
| [JdaieLin/cardputer-xiaozhi](https://github.com/JdaieLin/cardputer-xiaozhi)（CardputerZero 应用商店 `cardputerzero-xiaozhi` 0.2.6） | C++ **+ Python sidecar** | C++ 主程序（SDL2 音频、fbdev/SDL UI、evdev），**WebSocket+Opus 在 `ws_bridge.py`、显示在 `display_bridge.py`、MCP 在 `mcp_tools.py`**，stdin/stdout JSON 管道 | WebSocket、OTA 激活、MCP（local_command/web_search/camera）、服务端 VAD、连续对话 | 无 | RPi Zero 2 W (CM0) arm64 Debian，`.deb` | MIT | **核心网络/编解码是 Python，不满足"非 Python"要求**；其 `ui.cpp` 状态机、fbdev 渲染、push-to-talk（空格键）交互设计可参考；CardputerZero 是 1 GB RAM 的 ARM64，资源约束与 C1 不同 |
| [kiloGrand/xiaozhi-tspi](https://github.com/kiloGrand/xiaozhi-tspi) | C++11/14 | 100ask 的 CMake 重构版：ctrl_center / sound / gui(CLI) / qt_gui；ALSA、opus、speex resampler、TLS WebSocket、nlohmann json、gtest | WebSocket、Opus、**MCP server（2024-11-05）** | 无 | RK3566 泰山派 / Ubuntu | MIT | 比 100ask 原版整洁，MCP 实现可参考 |
| [Yinyifeng18/xiaozhi-linux-rk3568](https://github.com/Yinyifeng18/xiaozhi-linux-rk3568) | C/C++ | 100ask 派生，LVGL GUI；speexdsp、websocketpp、libwebsockets、curl、OpenSSL、Boost | 同 100ask | 无 | RK3568 | 未声明 | 无新增价值 |
| [Swair/cc-xiaozhi](https://github.com/Swair/cc-xiaozhi) | C++ | 单仓 `xiaozhiai/` + `cill/`，CMake/Make | 未详述 | — | Linux 桌面 | Apache-2.0 | 4 次提交，早期 |
| [justa-cai/xiaozhi-linux](https://github.com/justa-cai/xiaozhi-linux) | C | 单进程 + cJSON | 未详述 | — | Linux | 未声明 | 3 次提交，教育用途 |
| [Kevincoooool/xiaozhi-linux-t113](https://github.com/Kevincoooool/xiaozhi-linux-t113)、[fuqin123/Linux_xiaozhi_ai](https://github.com/fuqin123/Linux_xiaozhi_ai)、[luozhehao/xiaozhi-linux-imx6ull](https://github.com/luozhehao/xiaozhi-linux-imx6ull) | C | 100ask 派生 / 板级适配 | 同 100ask | 无 | T113 / IMX6ULL | — | 板级适配示例 |
| [zhulige/xiaozhi-sharp](https://github.com/zhulige/xiaozhi-sharp) | C# | .NET SDK + Client | WebSocket/MQTT | — | 多平台 | — | .NET 不上 MIPS |
| [kholile14/jtxiaozhi-client](https://github.com/kholile14/jtxiaozhi-client) | C++ Qt/QML | 桌面客户端 | 未详述 | — | Win/mac/Linux | MIT | Qt 不适用 |
| [coloz/xiaozhi-arduino](https://github.com/coloz/xiaozhi-arduino) | C++ (Arduino) | 「保留语音会话必需的协议与状态逻辑，显示/板卡/算法改成可选适配层」 | WebSocket | — | ESP32 Arduino | — | 思路与本方案一致：**剥离协议+状态机核心，其余做适配层**；可对照其裁剪清单 |
| [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)（上游，本地副本 `../xiaozhi-esp32` @ 5d54beb7） | C++ (ESP-IDF 6) | 单进程多任务 | 全部（WS、MQTT+UDP、MCP、OTA/激活、Glyph Push、ESP-SR 唤醒/AEC） | ESP-SR | ESP32 全系 | MIT | **移植源** |

**没有发现任何现成的 MIPS / 墨水屏 / 50 MiB 级别的小智 Linux 客户端。** 所有 Linux 版都基于 ARM Linux 单板（≥ 256 MB RAM）+ 动态链接的桌面级依赖（Boost/OpenSSL/curl/Qt/LVGL）。因此本项目需要自己做一遍"上游核心 + 轻量依赖"的移植，见 [PLAN.md](PLAN.md)。

## 2. 各项目细节

### 2.1 100askTeam/xiaozhi-linux（韦东山）

- 目录：`sound_app/`（`aplay.cpp`、`record.cpp`、`opus.cpp`、`ipc_udp.cpp`）、`control_center/`（`websocket_client.cpp`、`http.cpp`、`uuid.cpp`、`json.hpp`=nlohmann）、`gui/`（LVGL 8 + `lv_100ask_xz_ai`）。
- 链接：`sound_app`: `-lasound -lopus -lspeexdsp`；`control_center`: `-lboost_system -lssl -lcrypto -lcurl`。
- 板级适配放在独立仓库 [xiaozhi-linux-SupportList](https://github.com/100askTeam/xiaozhi-linux-SupportList)（Buildroot/Tina）。
- 文档站：<https://xiaozhi-linux.100ask.net/>。
- 284 星，最后推送 2025-07-08。

### 2.2 haoyn231/xiaozhi_linux_rs

- Cargo 依赖：`alsa 0.9`、`opus 0.3`、`tokio 1(full)`、`tokio-tungstenite 0.28 (rustls-webpki-roots)`、`reqwest 0.12 (rustls)`、`serde/serde_json`、`uuid v4`、`mac_address`；build.rs 用 autotools 静态编译 opus/speexdsp。
- 验证过 x86_64、aarch64、armv7 glibc、armv7 **uClibc**（Luckfox Pico RV1106 为主要目标）。
- 明确不做：本地唤醒词、AEC、OTA。
- 配置：编译期 `config.toml` + 运行期 `xiaozhi_config.json`。

### 2.3 JdaieLin/cardputer-xiaozhi（CardputerZero）

- `main/src/`：`application.cpp`、`ui.cpp`（状态机）、`ui_fbdev.cpp`、`ui_sdl.cpp`、`hal_evdev.cpp`、`hal_sdl.cpp`、`audio_pipeline_sdl.cpp`、`ws_client.cpp`、`ota_client.cpp`、`display_bridge.cpp`、`config.cpp`、`main_device.cpp`、`main_sim.cpp`。
- `main/tools/`：`display_bridge.py`（PIL/cairosvg 渲染 framebuffer）、`ws_bridge.py`（websockets + opus）、`mcp_tools.py`。
- 构建：SCons + `build.sh`，Zig 交叉到 aarch64，macOS 模拟器。依赖 `libsdl2 libsdl2-ttf libopus0 fonts-noto-cjk python3-pil python3-cairosvg`。
- 功能：双向语音、CJK+emoji、水彩律动 UI（GPLv3 组件，仅 aarch64）、OTA 激活码绑定 xiaozhi.me、空格键 push-to-talk、MCP 白名单命令执行、DuckDuckGo/Google News 搜索、CSI 相机视觉。
- 内存：水彩渲染在可用内存 < 64 MB(soft)/32 MB(critical) 时暂停——说明它面向 ≥ 512 MB 设备。
- 通过 [CardputerZero/packages](https://github.com/CardputerZero/packages) PR #146/#147 发布 `cardputerzero-xiaozhi 0.2.5/0.2.6-m5stack1`（arm64 .deb）。
- CardputerZero 硬件：Raspberry Pi CM0（BCM2710，4×A53，512 MB/1 GB），Debian，见 [CNX](https://www.cnx-software.com/2026/05/25/cardputerzero-a-raspberry-pi-cm0-pocket-computer-for-makers/)。

### 2.4 其它相关

- Go 生态只有**服务端**（`hackers365/xiaozhi-esp32-server-golang`、`zhangyujian111/xiaozhi-server-go`、`xdimtech/go-xiaozhi`、`zkhsko/xiaozhi-esp32-golang-server`），可用于自建服务器测试。
- Python 客户端 `py-xiaozhi` 与 `xiaozhi-esp32-server` 是社区事实标准，用来核对协议行为（如 `listen/detect` 文本输入）。

## 3. 芯片 / 工具链事实

- Ingenic X1600/X1600E：XBurst1 1.0 GHz + XBurst0，硬件 FPU，16K/16K L1 + 128K L2，SIP DDR2/LPDDR2 16–128 MB（X1600 32 MB，**X1600E 64 MB**），I2S，USB 2.0 OTG，硬件 RSA/AES，BGA-159。来源：<https://en.ingenic.com.cn/products-detail/id-16.html>。C1 Slim 实测约 50 MiB 可用，对应 X1600E。
- 君正官方工具链：`mips-linux-gnu-ingenic-gcc7.2.0-glibc2.29-fp64`（<https://www.ingenic.com.cn/news-detail/nid-350.html>）。本项目不依赖它：静态链接后 libc 无关。
- Rust MIPS：2023 年降为 Tier 3（[rust-lang/rust#115238](https://github.com/rust-lang/rust/pull/115238)），需 `-Zbuild-std`；rustls 在 `mipsel-unknown-linux-musl` 有失败记录（[rustls#1883](https://github.com/rustls/rustls/issues/1883)）。
- libopus 自带 MIPS 定点优化路径（`silk/mips/`、`celt/mips/`），配合 `--enable-fixed-point`。
- 唤醒词候选：[microWakeWord](https://github.com/kahrendt/microWakeWord)（TFLite-Micro，ESP32-S3 级别可跑，MIPS 1 GHz 可行）、[openWakeWord](https://github.com/dscripka/openWakeWord)（需 ONNX Runtime，对 MIPS/50 MiB 太重）、Porcupine（商业，无 MIPS）、Snowboy（已停止维护）。

## 4. 本地已有可复用资产（`../C1-Slim-Ports`、`../C1auncher`）

| 资产 | 位置 | 用途 |
| --- | --- | --- |
| 1bpp 帧 + `C1FONT1` 位图字体加载/绘制 | `C1-Slim-Ports/C1Home/src/c1gfx.h`、`tools/build_font.py` | 显示层基础（注意 GPL-3.0） |
| 墨水屏刷新策略与驱动逆向 | `C1-Slim-Ports/docs/EPAPER_REFRESH.md` | 刷新节流、全刷时机、用户态局刷（进阶） |
| ALSA 播放 / 混音器 | `C1auncher/Pinao/audio_device_linux_mipsle.go`、`App/music-player/volume_device.go` | `hw:0,0` 参数、`DAC Playback Volume` 0–158 |
| evdev 独占、app lease、SIGSTOP 桥 | `C1auncher/Pinao/platform_device_linux_mipsle.go`、`C1ancher/src/platform/app_lease.*`、`scripts/device-control.sh` | 启动器集成 |
| Wi-Fi 服务 | `C1auncher/C1ancher/src/services/wifi.c` | wpa_supplicant 控制套接字用法、`wlan0` 状态 |
| Zig / GCC 交叉构建脚本 | `C1-Slim-Ports/C1Sudoku/scripts/build.ps1`、`C1Home/build.sh` | 工具链参数 |
| QEMU-user 自测 | `C1-Slim-Ports/C1MicroPython` | 无真机时的冒烟测试方法 |
