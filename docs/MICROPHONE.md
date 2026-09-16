# C1 Slim 麦克风：原理、逆向证据与接入方案

> 来源：对 `eggfly/C1-Slim-Backup` release `backup-20260915` 的静态逆向（2026-09-16）。
> 分析对象：`/usr/bin/AudioRecorder`、`/usr/bin/d261/mpenMain`、`/etc/asound.conf`、运行时设备树、内核 `uImage`（p1 分区）。
> 方法：pyelftools 读符号表与 GOT，capstone 反汇编 MIPS32，GOT→dynsym 映射校验每一次库调用。
> **全部结论来自二进制与设备树，没有在真机上执行过录音。** 真机验证清单见 [DEVICE_PROBE.md](DEVICE_PROBE.md)。

---

## 0. 结论

**麦克风存在，通路完整，而且原厂就是用标准 ALSA 以 16 kHz 单声道 S16_LE 从 `hw:0,0` 采集的——这正是小智需要的格式，不需要任何重采样。**

更关键的一点：**原厂录音程序自始至终没有调用任何 ALSA mixer 接口**（`.dynsym` 里只有 `snd_pcm_*`，没有 `snd_mixer_*` / `snd_ctl_*`），`/var/lib/alsa/asound.state` 也是空的。
这说明采集通路在内核 ES8326 驱动 probe 时就已经配置好，**用户态打开 PCM 就能录音，不需要先设置任何控件**。

PLAN.md 里唯一的阻塞项（"是否有麦克风"）就此解除。

---

## 1. 硬件链路

```
麦克风(模拟)
   └─> ES8326 codec (Everest Semiconductor, I2C 地址 0x18, 总线 @0x10051000)
         MIC1..MIC4 输入，everest,mic1-src = <0>  → 选 MIC1
         MICBIAS1 / MICBIAS2 偏置
         内置 ADC + PGA + ALC(自动电平控制)
   └─> I2S  (pinctrl aic-pb-es8326: GPB21 + GPB25..GPB28)
   └─> AIC 控制器 ingenic,x1600-aic @0x10079000 (中断 0, DMA tx=0x3e rx=0x3f)
   └─> ingenic DMA
   └─> ASoC 机器驱动 snd_halley6_es8326  (compatible ingenic,x1600-sound, model "halley6")
   └─> ALSA card 0
         device 0 = "i2s-ecodec"  ES8326 HiFi     播放 + **采集**
         device 1 = "i2s-tloop"   pcm-dump        仅采集 = 发送回环(播放参考信号)
```

相关 GPIO（设备树）：

| 用途 | 属性 | GPIO | 极性 |
| --- | --- | --- | --- |
| codec 电荷泵使能 | `ingenic,cpvdd-en-gpio` | GPB23 | 高有效 |
| 音频通路选择 | `ingenic,audio-select-gpios` | GPB22 | **低有效** |
| 喇叭功放使能 | `ingenic,spken-gpios` | GPC25 | 高有效 |
| 耳机检测 | `hp_dete` 节点 | — | — |

内核里这几个驱动都是**内建**（`lib/modules/5.10.186/modules.builtin`）：
`asoc-dma`、`asoc-aic`、`asoc-i2s`、`asoc-i2s-tloop`、`ecodec/es8326`、`boards/snd_halley6_es8326`、`icodec/dump`。
不需要 insmod，开机即在。

### 1.1 `i2s-tloop` 是个惊喜

第二个 PCM 设备把**正在播放的信号**回环成一路采集流。这是硬件提供的 AEC 参考信号。
有了它，理论上可以做设备侧回声消除，进而支持小智的 realtime 全双工模式，而不是只能半双工。
上游 ESP32 版在没有 AFE 的芯片上也只能半双工，所以这一条是 C1 Slim 相对 ESP32-C3 类设备的**优势**，不是短板。
代价是要自己接一个 AEC（speexdsp 的 `speex_echo_*` 或 WebRTC APM），CPU 预算需要实测。列为后期可选项，不进 MVP。

---

## 2. 逆向证据：原厂到底怎么录音

### 2.1 `/etc/asound.conf`（设备上的明文配置）

```
pcm.!default { type plug; slave.pcm "dmix" }
ctl.!default { type hw; card 0 }
pcm.capture  { type plug; slave.pcm "hw:0,0" }
```

采集走 `hw:0,0`，播放默认经 `dmix` 混音。

### 2.2 `/usr/bin/AudioRecorder`（59 KB，独立进程）

C++ 写的独立录音进程，动态链接 `libasound.so.2` + ffmpeg 全家桶（`libavcodec/libavformat/libswresample`）+ `libffplay.so`。
`.dynsym` 暴露出完整类结构（未混淆的 Itanium ABI 名字）：

| 类 | 方法 |
| --- | --- |
| `AlsaRecord` | `openAudio(std::string, int, int)`、`setupHandle()`、`readAudio(void*, size_t)`、`recordAudio(void*, size_t)`、`getSampleRate()`、`getChannel()`、`closeAudio()` |
| `FFmpegRecorder` | `start()`、`stop()`、`startListen()`、`recordThread()`、`ListenThread(void*)`、`set_device()`、`calculatePcmDB(unsigned char const*, unsigned int)`、`getAudioDB()`、`setOutputFile(std::string)` |
| `AudioEncoder` | `setupEncoder(AVCodecID)`、`allocFrame()`、`write(AVFrame*, AVPacket*)`、`setSampleRate(int)`、`setChannel(int)`、`setOutFile(std::string)`、`endEncode()` |

源码路径残留：`ffmpegRecorder/AlsaRecord.cpp`、`ffmpegRecorder/AudioEncoder.cpp`、`ffmpegRecorder/main.cpp`。

### 2.3 设备名与参数（反汇编 `FFmpegRecorder::set_device`，0x404cd0）

GOT 基址解析为 `0x410000`，两个字符串常量落在 `.rodata`：

```
0x0040aa68  "hw:0,0"
0x0040aaa0  "default"
```

调用点：

```asm
0x00404de4  addiu $a3, $zero, 0x3e80   ; 16000
0x00404de8  addiu $a2, $zero, 1        ; 1
0x00404dec  move  $a1, $s3             ; std::string = "hw:0,0"
0x00404df0  bal   0x405ab4             ; AlsaRecord::openAudio(string, int, int)
```

`openAudio` 把 `$a3` 存进 `this+0x1c`、`$a2` 存进 `this+0x20`，随后 `setupHandle` 把 `+0x1c` 喂给 `set_rate_near`、`+0x20` 喂给 `set_channels_near`。
**即：采样率 16000，声道 1。** 打开失败时用 `"default"` 再试一次。

### 2.4 完整 ALSA 序列（反汇编 `AlsaRecord::setupHandle`，0x405664）

每一个 `lw $t9, -0xXXXX($gp)` 都通过 `DT_MIPS_GOTSYM` / `DT_MIPS_LOCAL_GOTNO` 映射回了 dynsym，逐条核对无误：

```c
snd_pcm_open(&handle, "hw:0,0", SND_PCM_STREAM_CAPTURE /* 1 */, 0);
snd_pcm_hw_params_malloc(&params);
snd_pcm_hw_params_any(handle, params);
snd_pcm_hw_params_set_access(handle, params, SND_PCM_ACCESS_RW_INTERLEAVED /* 3 */);
snd_pcm_hw_params_set_format(handle, params, SND_PCM_FORMAT_S16_LE /* 2 */);
snd_pcm_hw_params_set_rate_near(handle, params, &rate /* 16000 */, 0);
snd_pcm_hw_params_set_channels_near(handle, params, &channels /* 1 */);
snd_pcm_hw_params(handle, params);
snd_pcm_hw_params_free(params);
snd_pcm_prepare(handle);
```

**没有 period size / buffer size 设置**，全部用驱动默认值。也**没有任何 mixer 调用**。

### 2.5 读取循环（反汇编 `AlsaRecord::readAudio`，0x405b28）

两个值得照抄的工程细节：

1. **开头 3072 帧被强制静音。** 计数器 `this+0x24` 小于 `0xc00`(3072) 时，直接 `memset(buf, 0, frames*channels*2)` 并累加计数，不使用真实数据。
   3072 帧 @ 16 kHz = **192 ms**。这是在压 ES8326 ADC 的上电瞬态（爆音/直流漂移）。
2. **错误恢复**（注意 MIPS 的 errno 编号与 x86 不同）：

   | 返回值 | MIPS errno | 处理 |
   | --- | --- | --- |
   | `-32` | `-EPIPE` | `snd_pcm_prepare()` 后重试，最多 3 次 |
   | `-92` | `-ESTRPIPE`（MIPS 上是 92，不是通用的 86） | 退出返回已读字节 |
   | `-81` | `-EBADFD` | 退出返回已读字节 |
   | `0 < n < 请求量` | 部分读 | 累加并继续读 |

### 2.6 电平检测（反汇编 `FFmpegRecorder::calculatePcmDB`，0x4045e0）

```c
// 逐个 int16 取绝对值求和，除以样本数，再取对数
double mean = sum(|s16[i]|) / (len / 2);
int db = mean > 0 ? (int)(20.0 * log10(mean)) : 0;   // 常量 20.0 位于 .rodata 0x40ab30
```

注意是**平均绝对值**而不是 RMS，满刻度约 `20*log10(32768) ≈ 90`。
原厂用它做静音检测和"说话中"指示（日志里有 `db:%d`、`before stop n:%d,db:%d,passTime:%lld`）。
我们做自动停止监听（auto listening）时可以直接沿用这个便宜的度量。

### 2.7 编码能力

`AudioEncoder::setupEncoder(AVCodecID)` 走 ffmpeg，`.rodata` 里有 `"opus"` 和 `The output file format is %s.`。
设备 `/usr/lib` 里确实带着 **libopus 1.4**（`libopus.so.0.9.0`，326 KB），`libavcodec.so.58` 的 `DT_NEEDED` 包含 `libopus.so.0`，字符串里有 `libopus` 编码器与 `libopusdec` 解码器。

**结论：这颗 1 GHz 单核 MIPS 上跑 Opus 编解码是原厂已经在做的事**，不是我们要冒的风险。

### 2.8 `mpenMain` 侧的用法

`mpenMain` 自己**不链接 libasound**，录音全部委托给 `AudioRecorder` 子进程：

- `socket_audio_recorder_start(char const*, std::function<void(int)> const&, std::function<void(int)> const&, int)`
- `socket_audio_recorder_stop_()`
- 源文件 `SmartHome/src/common/media_player/audio_recorder.cpp`
- 用 shell 起进程：`.rodata` 里有 `"AudioRecorder&"` 和 `"killall -9 AudioRecorder"`
- 进程间用 **SysV 消息队列**（`msgget`/`msgsnd`/`msgrcv`，变量 `msqidsend`/`msqidrecv`）传控制命令与 dB 电平，另有 `/tmp/fifo_%05d` 命名管道和 `127.0.0.1` socket 传 PCM
- `AudioRecorder` 支持 `pcm_data_fd` 参数，可以把裸 PCM 直接写到一个 fd（`outputpcm` 标志、`direct` 模式）

调用它的上层是口语评测与语音识别：
`MPScan::IflyOralEvalEngine::startAudioRecorder(shared_ptr<OralEvalContext> const&, int)`、
`MPScan::TuringOralEvalEngine`、`MPScan::TuringOnlineASREngineWS`（WebSocket 语音识别），
录音落盘路径包括 `/storage/asr.wav`。工厂测试页里也有 `onGCTestClickRecord` 录音自检项。

---

## 3. 可用的 ALSA 控件

从内核镜像（p1 uImage → gzip@0x4230 → 9.5 MB vmlinux）里提取到的控件名与 DAPM 部件：

**采集相关控件**

| 控件 | 说明 |
| --- | --- |
| `ADC Capture Volume` | 采集主音量 |
| `ADC PGA Gain Volume` | 麦克风前置放大增益 |
| `ADC PGA Volume` | PGA 音量 |
| `ALC Capture Switch` | 编解码器内置自动电平控制开关 |
| `ALC Capture Target Level` | ALC 目标电平 |
| `ALC Capture Recovery Level` | ALC 恢复电平 |
| `ALC Capture Winsize` | ALC 窗口 |

**播放相关**：`DAC Playback Volume`（就是 C1ancher 音乐播放器用的 `numid=1`，范围 0–158）

**DAPM 部件**：`MIC1` `MIC2` `MIC3` `MIC4`、`Microphone`、`MICBIAS1` `MICBIAS2`、`Speaker`、`Speaker Enable`、`Headphone`、`HPOL` `HPOR`、`Left DAC` `Right DAC`

ES8326 自带 ALC，对语音场景很友好。原厂没动这些控件，用的是驱动默认值；我们默认也不动，只在真机实测信噪比不佳时再调 `ADC PGA Gain Volume` 和 ALC 三项。

### 3.1 双麦克风？

`AudioRecorder` 的 `.rodata` 里有一句 `dualMicProcess is skip`，codec 也支持 MIC1–MIC4，但设备树只配了 `everest,mic1-src = <0>`，而且原厂代码明确跳过了双麦处理。
**倾向判断是单麦克风**，双麦代码是同一套 SDK 在别的机型上用的。真机 `arecord --dump-hw-params` 看最大声道数即可定论。

---

## 4. 对 C1SlimXiaoZhi 的方案

### 4.1 采集参数：直接对齐，零转换

| 项 | 小智上行要求 | C1 Slim 实际 | 结论 |
| --- | --- | --- | --- |
| 采样率 | 16000 | 16000（原厂即用） | 完全一致 |
| 声道 | 1 | 1（原厂即用） | 完全一致 |
| 格式 | S16 PCM | S16_LE | 完全一致 |
| 帧长 | 60 ms = 960 帧 | 自选 | 直接按 960 帧读 |

上行链路因此是：`ALSA hw:0,0 读 960 帧 → libopus 编码 → WebSocket 二进制帧`，中间**不需要任何重采样**。
PLAN.md 里原先预留的"48k→16k 抽取"分支可以删掉。

下行仍需处理：服务端 TTS 通常是 24 kHz，而播放侧原厂用 48 kHz 立体声。直接用 libopus 的 `opus_decoder_create(48000, ...)` 让解码器输出 48 kHz，再单声道复制成立体声即可，**同样不需要独立重采样器**。

### 4.2 实现选择：tinyalsa 还是 libasound

改变原计划。原方案打算用 tinyalsa 直接打 `/dev/snd/pcmC0D0c` 的 ioctl，现在有更稳的两条路：

| 方案 | 说明 | 取舍 |
| --- | --- | --- |
| **A. tinyalsa 静态链接**（推荐） | 自带 `pcm_open(0, 0, PCM_IN, &config)`，不依赖设备上的 libasound，符合"静态 ELF"约定 | 需要自己设 period/buffer；`hw:0,0` 等价于 card 0 device 0，绕过 `asound.conf` 的 plug 层，但反正格式已经原生匹配，不需要 plug 转换 |
| B. 复用设备自带 libasound | 和原厂代码路径 100% 一致，风险最低 | 破坏静态链接约定，要带 `/usr/lib/libasound.so.2` 依赖 |

采用 A，但**用 B 作为 Phase 0 的参照基准**：先用设备自带的 `/usr/bin/arecord` 验证硬件，再用 tinyalsa 复现同样的结果，两者比对。

tinyalsa 配置：

```c
struct pcm_config cfg = {
    .channels          = 1,
    .rate              = 16000,
    .format            = PCM_FORMAT_S16_LE,
    .period_size       = 960,   /* 60 ms，和 Opus 帧对齐 */
    .period_count      = 4,     /* 240 ms 缓冲，给单核抢占留余量 */
    .start_threshold   = 0,
    .stop_threshold    = 0,
    .silence_threshold = 0,
};
struct pcm *p = pcm_open(0 /* card */, 0 /* device */, PCM_IN, &cfg);
```

period/buffer 是原厂没设的部分，需要真机调。若 `pcm_open` 报参数不支持，退回 `period_size=1024, period_count=4`，在编码前自己重新切成 960 帧。

### 4.3 必须照抄的三个细节

1. **丢弃开头 192 ms。** 每次 `pcm_open` 后把最初 3072 帧当作静音（直接丢弃，或送静音给编码器）。这不是玄学，是原厂对 ES8326 上电瞬态的既定处理。副作用是按下说话键到真正开始采集有 192 ms 延迟，UI 上应先显示"正在聆听"再开始计时。
2. **EPIPE 恢复。** `pcm_read` 返回 `-EPIPE` 时 `pcm_prepare()` 重试，上限 3 次。单核设备上音频线程被显示写帧（685 ms 一帧！）挤掉导致 overrun 是常态，这条必须有。
3. **MIPS errno 不同。** `ESTRPIPE` 在 MIPS 上是 **92**，不是通用 Linux 的 86。写错误处理时不要照抄 x86 的常量表。

### 4.4 电平与自动停止

沿用原厂的便宜度量：`db = 20*log10(mean(|s16|))`，满刻度约 90。
用途：麦克风条形电平指示（墨水屏上画个静态格子，只在跨阈值时重绘，避免高频刷新）、静音超时自动停止监听（配合小智的 `listen.state=stop`）。
具体阈值需要真机采样安静环境与正常说话的 db 值后再定，不要凭空写死。

### 4.5 影响到的 PLAN.md 条目

- 阻塞项解除：Phase 0 从"决定方案走向"降级为"参数标定"。
- 上行重采样分支删除。
- 新增可选项：基于 `hw:0,1`（i2s-tloop）的设备侧 AEC → 小智 realtime 全双工模式。
- Opus 可行性风险下调：设备自带 libopus 1.4 且原厂在用。
- 分区表勘误：`mmcblk0p7` 是**根分区**（400 MB，ext4 只读，`root=/dev/mmcblk0p7`），不是"未挂载"。根分区仅剩约 149 MB 可用，我们的二进制应放 `/storage`。

---

## 5. 待真机确认

以下**没有**在真机上验证过，Phase 0 必须实测：

| 项 | 命令 | 预期 |
| --- | --- | --- |
| 采集设备存在 | `arecord -l` | 出现 card 0 device 0 |
| 是否有第二个 PCM（tloop） | `cat /proc/asound/pcm` | 期望看到 `00-01` capture |
| 支持的参数范围 | `arecord -D hw:0,0 --dump-hw-params -d 1 /dev/null` | rate 含 16000，channels 含 1，含 S16_LE；同时看最大声道数判断单/双麦 |
| 实际能录到人声 | `arecord -D hw:0,0 -f S16_LE -r 16000 -c 1 -d 5 /tmp/t.wav` 后 `adb pull` | 能听清说话，底噪可接受 |
| 默认控件值 | `amixer contents` | 记录 `ADC Capture Volume`、`ADC PGA Gain Volume`、ALC 各项的默认值 |
| 前 192 ms 现象 | 看 `/tmp/t.wav` 波形开头 | 确认是否有瞬态；原厂静音 192 ms 是否必要 |
| 静音/说话 dB | 用上面的公式算 | 定自动停止阈值 |

**在这些跑通之前，不要把"麦克风可用"写进 README 当作既成事实。** 目前的把握来自二进制证据，强，但不等于实测。
