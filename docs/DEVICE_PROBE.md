# Phase 0：C1 Slim 真机探测清单（只读）

前提：已获得 root ADB（见 `../C1-Slim-Ports/tools/root-adb/`）。以下命令**只读**，不修改系统分区。
把输出粘贴到本文档 §3，或保存到 `docs/probe/<日期>/`。

## 1. 必做（决定方案走向）

```bash
adb shell 'cat /proc/asound/cards; echo ---; cat /proc/asound/pcm; echo ---; ls -l /dev/snd; echo ---; arecord -l; echo ---; aplay -l'
```

```bash
adb shell 'arecord -D hw:0,0 --dump-hw-params -d 1 /dev/null 2>&1 | head -60'
```

```bash
adb shell 'amixer contents' 
```

```bash
adb shell 'arecord -D hw:0,0 -f S16_LE -r 16000 -c 1 -d 4 /tmp/rec16k.wav; arecord -D hw:0,0 -f S16_LE -r 48000 -c 2 -d 4 /tmp/rec48k.wav; ls -l /tmp/rec*.wav' && adb pull /tmp/rec16k.wav /tmp/rec48k.wav .
```

录音时对着设备说话，拉回电脑用 `afplay`/`ffplay` 听：**能否听到人声、底噪如何、是否需要打开某个 mixer 开关（`amixer contents` 里找 Capture/ADC/MIC 相关 numid）**。

## 2. 补充信息

```bash
adb shell 'cat /proc/cpuinfo; echo ---; free; echo ---; cat /proc/meminfo | head -8; echo ---; ls /sys/devices/system/cpu/cpu0/cpufreq/ 2>/dev/null && cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_frequencies /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor'
```

```bash
adb shell 'cat /proc/version; echo ---; zcat /proc/config.gz 2>/dev/null | grep -E "SND_USB|USB_AUDIO|CONFIG_MODULES|CRYPTO_USER_API|HIGH_RES|PREEMPT" ; ls /lib/modules 2>/dev/null'
```

```bash
adb shell 'date; echo ---; ls -la /etc/ssl /etc/ssl/certs /usr/share/ca-certificates 2>&1 | head -20; echo ---; cat /etc/resolv.conf; echo ---; ls /sys/class/rtc 2>&1'
```

```bash
adb shell 'ls /sys/class/power_supply/; for d in /sys/class/power_supply/*; do echo $d; cat $d/uevent 2>/dev/null; done'
```

```bash
adb shell 'cat /sys/class/net/wlan0/address; cat /sys/class/net/wlan0/carrier; ip addr show wlan0 | grep inet; wpa_cli -i wlan0 status 2>/dev/null | head; ls /var/run/wpa_supplicant/'
```

```bash
adb shell 'ls -l /usr/bin | grep -E "aplay|arecord|amixer|ffplay|ffmpeg|evtest|wpa_cli|ntpd|ntpdate|curl|wget|openssl"; ls /usr/lib | grep -E "asound|opus|ssl|crypto|avcodec|speex"'
```

按键码表（按每个键一次，Ctrl-C 结束；MIPS 上 `struct input_event` 为 16 字节：`tv_sec u32, tv_usec u32, type u16, code u16, value s32`）：

```bash
adb shell 'hexdump -e "4/4 \"%08x \" \"\n\"" /dev/input/event0' 
```

```bash
adb shell 'hexdump -e "4/4 \"%08x \" \"\n\"" /dev/input/event1'
```

原厂 UI 内存占用与进程列表（用于判断 SIGSTOP 共存时的内存余量）：

```bash
adb shell 'ps -o pid,rss,vsz,comm 2>/dev/null || ps; echo ---; cat /proc/$(pidof mpenMain)/status | grep -E "VmRSS|VmSize"'
```

## 3. 结果记录

| 项目 | 结果 | 日期 |
| --- | --- | --- |
| capture 设备存在？ | | |
| 支持采样率 / 声道 / 格式 | | |
| 录音可听？需要哪些 mixer 设置 | | |
| 播放设备参数 | | |
| free（总/可用） | | |
| cpufreq | | |
| CA 证书 | | |
| 时钟 / RTC | | |
| 电池 sysfs 路径 | | |
| wlan0 MAC | | |
| 按键码：Enter/OK、Back/Esc、Home、上下左右、音量+/-、电源、字母区 | | |
| mpenMain RSS | | |
| USB audio 内核支持 | | |

## 4. 判定

- capture 可用且录音清晰 → 走 PLAN.md 主线（Phase 1）。
- capture 不存在 → 检查 USB audio 内核支持；同时启动 PLAN.md §7 的键盘文字对话路线。
