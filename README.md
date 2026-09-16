# C1SlimXiaoZhi

把官方 [小智 AI 聊天机器人（xiaozhi-esp32）](https://github.com/78/xiaozhi-esp32) 移植到快易典 C1 Slim / MP-D261：
君正 X1600E（MIPS32r2，1 GHz 单核，约 50 MiB RAM）、Linux 5.10、296×152 黑白墨水屏、实体键盘。

一个**静态链接的 MIPS Linux ELF**，C++ 写成，不依赖 Python，不依赖设备上任何动态库。
产出的二进制在架构标志、ABI、浮点模式上与原厂 `mpenMain` 完全一致。

```
build/device/c1xiaozhi: ELF32 MIPS 小端, 0x70001005 (o32, mips32r2), 硬浮点双精度, 静态
```

## 状态

**代码写完了，能编译，46 项主机单元测试全过，但从未在真机上运行过。**

逐项的「已实现 / 验证到什么程度」清单见 [docs/STATUS.md](docs/STATUS.md)。真机第一步也在那里。

## 已实现

对齐 ESP32 版的部分：OTA 与设备激活、WebSocket 与 MQTT+UDP 两种传输（含 UDP 音频的
AES-128-CTR 加密与序号防重放）、三种二进制协议版本、Opus 语音、完整的消息类型、
设备状态机（逐字移植）、设备侧 MCP（JSON-RPC 2.0，含分页）、提示音、设置持久化、自更新。

针对本设备重写的部分：1bpp 墨水屏渲染与刷新节流、Unifont 中文字体、evdev 按键、
tinyalsa 音频、启动器共存、Wi-Fi 与电池状态。

不做的部分：离线唤醒词（改用按键 push-to-talk）、设备侧回声消除、摄像头。
原因见 [docs/STATUS.md](docs/STATUS.md) §5。

## 构建

需要 [zig](https://ziglang.org/)（0.14+）、CMake 3.20+、Ninja、Python 3。

```bash
tools/fetch-deps.sh     # 下载并校验 opus / mbedtls / cJSON / tinyalsa / unifont / CA 根证书
tools/build.sh          # 交叉编译设备版，自带 ELF 属性校验
tools/build.sh host test # 主机版 + 跑单元测试
```

依赖是**固定版本的发布包加 SHA-256 校验**，不是 git 子模块，构建可复现也不依赖克隆大仓库。

## 安装

需要已开启 root ADB 的设备，方法见 [C1-Slim-Ports 的 root ADB 指南](https://github.com/eggfly/C1-Slim-Ports/tree/main/tools/root-adb)。

```bash
tools/deploy.sh
adb shell '/storage/c1/xiaozhi/c1xiaozhi --log /storage/c1/xiaozhi/run.log'
```

所有文件都在 `/storage/c1/xiaozhi` 下。不动只读根分区，不改启动脚本，删掉目录即卸载。

程序会接管屏幕和键盘、暂停原厂界面，长按 Home 两秒或收到 SIGTERM 时恢复两者。

## 文档

- [docs/STATUS.md](docs/STATUS.md) — 实现状态，验证程度，真机第一步，设计取舍
- [docs/PLAN.md](docs/PLAN.md) — 移植方案与架构
- [docs/MICROPHONE.md](docs/MICROPHONE.md) — 麦克风硬件原理与原厂录音程序逆向
- [docs/RESEARCH.md](docs/RESEARCH.md) — 现有 Linux / CardputerZero 小智客户端调研
- [docs/DEVICE_PROBE.md](docs/DEVICE_PROBE.md) — 真机探测与标定清单

## 相关仓库

- [eggfly/C1SlimXiaoZhi](https://github.com/eggfly/C1SlimXiaoZhi) — 本仓库
- [eggfly/C1-Slim-Ports](https://github.com/eggfly/C1-Slim-Ports) — 设备资料、墨水屏逆向、已移植应用
- [fwz233-RE/C1auncher](https://github.com/fwz233-RE/C1auncher) — 设备端启动器与应用开发文档
- [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) — 上游

## 许可

本仓库自研代码待定（建议 MIT，与上游一致）。第三方组件遵循各自许可：
libopus（BSD）、Mbed TLS（Apache-2.0）、cJSON（MIT）、tinyalsa（BSD）、
GNU Unifont（GPLv2+ 带字体嵌入例外）、Mozilla CA 证书包（MPL-2.0）。
