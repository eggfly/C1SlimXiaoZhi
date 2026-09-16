# C1SlimXiaoZhi

把官方 [小智 AI 聊天机器人（xiaozhi-esp32）](https://github.com/78/xiaozhi-esp32) 移植到快易典 C1 Slim / MP-D261：
君正 X1600E（MIPS32r2，1 GHz 单核，约 50 MiB RAM）、Linux 5.10、296×152 黑白墨水屏、实体键盘。

目标是一个**静态链接的 MIPS Linux ELF**（C++，无 Python、无动态库），尽量完整地对齐 ESP32 版功能：
OTA 激活、WebSocket 与 MQTT+UDP 两种传输、Opus 语音、设备侧 MCP、状态机、提示音、多语言；
显示层改为 1bpp 墨水屏渲染，唤醒改为按键 push-to-talk（离线唤醒词为后期可选项）。

## 当前状态

方案阶段，尚未开始编码。请先阅读：

- [docs/PLAN.md](docs/PLAN.md) — 移植方案、架构、依赖、分阶段计划与验收标准（给执行者的蓝图）
- [docs/RESEARCH.md](docs/RESEARCH.md) — 现有 Linux / CardputerZero 小智客户端调研与结论
- [docs/MICROPHONE.md](docs/MICROPHONE.md) — 麦克风硬件原理与原厂录音程序逆向：ES8326 通路、精确的 ALSA 调用序列、可用控件、接入方案
- [docs/DEVICE_PROBE.md](docs/DEVICE_PROBE.md) — 真机探测与标定清单

麦克风通路已从原厂二进制逆向确认：ES8326 codec，ALSA `hw:0,0`，**16 kHz 单声道 S16_LE**，
与小智上行要求逐项一致，不需要重采样。真机录音尚未实测。

## 相关仓库

- [eggfly/C1SlimXiaoZhi](https://github.com/eggfly/C1SlimXiaoZhi) — 本仓库
- [eggfly/C1-Slim-Ports](https://github.com/eggfly/C1-Slim-Ports) — 设备资料、墨水屏逆向、已移植应用
- [fwz233-RE/C1auncher](https://github.com/fwz233-RE/C1auncher) — 设备端启动器与应用开发文档
- [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) — 上游

## 许可

待定（建议 MIT，与上游一致）。第三方组件遵循各自许可。
