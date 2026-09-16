#ifndef C1XZ_AUDIO_TINYALSA_AUDIO_CODEC_H
#define C1XZ_AUDIO_TINYALSA_AUDIO_CODEC_H

#include <mutex>
#include <vector>

#include "audio/audio_codec.h"

struct pcm;
struct mixer;

// ALSA backend for the C1 Slim, driven through tinyalsa so the binary stays
// static and does not need the device's libasound.
//
// The parameters come from reverse engineering the factory AudioRecorder; see
// docs/MICROPHONE.md for the evidence behind each one.
class TinyAlsaAudioCodec : public AudioCodec {
public:
    TinyAlsaAudioCodec();
    ~TinyAlsaAudioCodec() override;

    bool Start() override;
    void Stop() override;
    void EnableInput(bool enable) override;
    void EnableOutput(bool enable) override;
    int Read(int16_t* dest, int samples) override;
    int Write(const int16_t* data, int samples) override;
    void SetOutputVolume(int volume) override;

    // Card and device default to 0/0, matching the factory `hw:0,0`.
    void SetCard(int card) { card_ = card; }
    void SetCaptureDevice(int device) { capture_device_ = device; }
    void SetPlaybackDevice(int device) { playback_device_ = device; }

    // Frames discarded at the start of every capture session. The factory
    // recorder zeroes the first 3072 frames, which is 192 ms at 16 kHz, to hide
    // the ES8326 ADC power-up transient.
    void SetCaptureWarmupFrames(int frames) { warmup_frames_ = frames; }

private:
    int card_ = 0;
    int capture_device_ = 0;
    int playback_device_ = 0;
    int warmup_frames_ = 3072;

    int period_size_ = 960;   // 60 ms at 16 kHz, one Opus frame
    int period_count_ = 4;    // 240 ms of slack for a busy single core

    pcm* capture_ = nullptr;
    pcm* playback_ = nullptr;
    mixer* mixer_ = nullptr;

    std::mutex capture_mutex_;
    std::mutex playback_mutex_;
    int warmup_remaining_ = 0;
    std::vector<int16_t> interleave_buffer_;

    bool OpenCapture();
    void CloseCapture();
    bool OpenPlayback();
    void ClosePlayback();
};

#endif  // C1XZ_AUDIO_TINYALSA_AUDIO_CODEC_H
