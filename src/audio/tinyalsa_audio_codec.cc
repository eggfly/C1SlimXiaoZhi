#include "audio/tinyalsa_audio_codec.h"

#include <tinyalsa/mixer.h>
#include <tinyalsa/pcm.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "platform/log.h"

#define TAG "Alsa"

namespace {

// The ES8326 playback path the factory uses: 48 kHz stereo. Capture is 16 kHz
// mono, which is exactly what the uplink wants.
constexpr int kPlaybackRate = 48000;
constexpr int kPlaybackChannels = 2;
constexpr int kCaptureRate = 16000;
constexpr int kCaptureChannels = 1;

// "DAC Playback Volume" on this codec runs 0..158; the factory music player
// uses the same scale.
constexpr int kMaxMixerVolume = 158;

}  // namespace

TinyAlsaAudioCodec::TinyAlsaAudioCodec() {
    input_sample_rate_ = kCaptureRate;
    input_channels_ = kCaptureChannels;
    output_sample_rate_ = kPlaybackRate;
    output_channels_ = 1;  // callers hand us mono; we interleave to stereo
}

TinyAlsaAudioCodec::~TinyAlsaAudioCodec() { Stop(); }

bool TinyAlsaAudioCodec::Start() {
    mixer_ = mixer_open(static_cast<unsigned int>(card_));
    if (mixer_ == nullptr) {
        C1XZ_LOGW(TAG, "could not open the mixer for card %d; volume control is unavailable",
                  card_);
    }
    SetOutputVolume(output_volume_);
    return true;
}

void TinyAlsaAudioCodec::Stop() {
    EnableInput(false);
    EnableOutput(false);
    if (mixer_ != nullptr) {
        mixer_close(mixer_);
        mixer_ = nullptr;
    }
}

bool TinyAlsaAudioCodec::OpenCapture() {
    pcm_config config;
    memset(&config, 0, sizeof(config));
    config.channels = kCaptureChannels;
    config.rate = kCaptureRate;
    config.period_size = static_cast<unsigned int>(period_size_);
    config.period_count = static_cast<unsigned int>(period_count_);
    config.format = PCM_FORMAT_S16_LE;
    config.start_threshold = 0;
    config.stop_threshold = 0;
    config.silence_threshold = 0;

    capture_ = pcm_open(static_cast<unsigned int>(card_),
                        static_cast<unsigned int>(capture_device_), PCM_IN, &config);
    if (capture_ == nullptr || pcm_is_ready(capture_) == 0) {
        C1XZ_LOGE(TAG, "cannot open capture hw:%d,%d: %s", card_, capture_device_,
                  capture_ != nullptr ? pcm_get_error(capture_) : "no device");
        if (capture_ != nullptr) {
            pcm_close(capture_);
            capture_ = nullptr;
        }
        return false;
    }
    // The factory recorder zeroes the first 3072 frames of every session. The
    // ES8326 ADC needs that long to settle; without it the first fifth of a
    // second is a thump that the server hears as speech.
    warmup_remaining_ = warmup_frames_;
    C1XZ_LOGI(TAG, "capture open: hw:%d,%d %d Hz mono, %d frame periods", card_, capture_device_,
              kCaptureRate, period_size_);
    return true;
}

void TinyAlsaAudioCodec::CloseCapture() {
    if (capture_ != nullptr) {
        pcm_close(capture_);
        capture_ = nullptr;
    }
}

bool TinyAlsaAudioCodec::OpenPlayback() {
    pcm_config config;
    memset(&config, 0, sizeof(config));
    config.channels = kPlaybackChannels;
    config.rate = kPlaybackRate;
    // 20 ms periods at 48 kHz; four of them give 80 ms of buffer, enough to
    // ride out a display refresh without adding noticeable latency.
    config.period_size = 960;
    config.period_count = 4;
    config.format = PCM_FORMAT_S16_LE;
    config.start_threshold = 0;
    config.stop_threshold = 0;
    config.silence_threshold = 0;

    playback_ = pcm_open(static_cast<unsigned int>(card_),
                         static_cast<unsigned int>(playback_device_), PCM_OUT, &config);
    if (playback_ == nullptr || pcm_is_ready(playback_) == 0) {
        C1XZ_LOGE(TAG, "cannot open playback hw:%d,%d: %s", card_, playback_device_,
                  playback_ != nullptr ? pcm_get_error(playback_) : "no device");
        if (playback_ != nullptr) {
            pcm_close(playback_);
            playback_ = nullptr;
        }
        return false;
    }
    C1XZ_LOGI(TAG, "playback open: hw:%d,%d %d Hz stereo", card_, playback_device_, kPlaybackRate);
    return true;
}

void TinyAlsaAudioCodec::ClosePlayback() {
    if (playback_ != nullptr) {
        pcm_close(playback_);
        playback_ = nullptr;
    }
}

void TinyAlsaAudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    if (enable == input_enabled_) {
        return;
    }
    if (enable) {
        input_enabled_ = OpenCapture();
    } else {
        CloseCapture();
        input_enabled_ = false;
    }
}

void TinyAlsaAudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(playback_mutex_);
    if (enable == output_enabled_) {
        return;
    }
    if (enable) {
        output_enabled_ = OpenPlayback();
    } else {
        ClosePlayback();
        output_enabled_ = false;
    }
}

int TinyAlsaAudioCodec::Read(int16_t* dest, int samples) {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    if (capture_ == nullptr || dest == nullptr || samples <= 0) {
        return -1;
    }

    // Recover from overruns the way the factory recorder does: re-prepare and
    // retry a bounded number of times. On this single core an overrun is a
    // normal consequence of the display blocking for 685 ms, not a fault.
    int attempts = 0;
    while (attempts < 3) {
        int rc = pcm_readi(capture_, dest, static_cast<unsigned int>(samples));
        if (rc > 0) {
            if (warmup_remaining_ > 0) {
                int muted = std::min(warmup_remaining_, rc);
                memset(dest, 0, static_cast<size_t>(muted) * sizeof(int16_t) * kCaptureChannels);
                warmup_remaining_ -= muted;
            }
            return rc;
        }
        if (rc == -EPIPE || rc == -ESTRPIPE) {
            // ESTRPIPE is 92 on MIPS, not the 86 used on x86; relying on the
            // constant rather than a literal keeps that correct.
            C1XZ_LOGW(TAG, "capture overrun, re-preparing");
            pcm_prepare(capture_);
            ++attempts;
            continue;
        }
        C1XZ_LOGE(TAG, "capture read failed: %s", pcm_get_error(capture_));
        return -1;
    }
    return 0;
}

int TinyAlsaAudioCodec::Write(const int16_t* data, int samples) {
    std::lock_guard<std::mutex> lock(playback_mutex_);
    if (playback_ == nullptr || data == nullptr || samples <= 0) {
        return -1;
    }

    // The conversation is mono but the hardware path is stereo, so duplicate
    // into both channels rather than opening a plug layer for it.
    interleave_buffer_.resize(static_cast<size_t>(samples) * kPlaybackChannels);
    for (int i = 0; i < samples; ++i) {
        interleave_buffer_[i * 2] = data[i];
        interleave_buffer_[i * 2 + 1] = data[i];
    }

    int attempts = 0;
    while (attempts < 3) {
        int rc = pcm_writei(playback_, interleave_buffer_.data(),
                            static_cast<unsigned int>(samples));
        if (rc >= 0) {
            return samples;
        }
        if (rc == -EPIPE || rc == -ESTRPIPE) {
            C1XZ_LOGW(TAG, "playback underrun, re-preparing");
            pcm_prepare(playback_);
            ++attempts;
            continue;
        }
        C1XZ_LOGE(TAG, "playback write failed: %s", pcm_get_error(playback_));
        return -1;
    }
    return -1;
}

void TinyAlsaAudioCodec::SetOutputVolume(int volume) {
    output_volume_ = std::max(0, std::min(100, volume));
    if (mixer_ == nullptr) {
        return;
    }
    mixer_ctl* ctl = mixer_get_ctl_by_name(mixer_, "DAC Playback Volume");
    if (ctl == nullptr) {
        C1XZ_LOGW(TAG, "no 'DAC Playback Volume' control on this card");
        return;
    }
    int min = mixer_ctl_get_range_min(ctl);
    int max = mixer_ctl_get_range_max(ctl);
    if (max <= min) {
        min = 0;
        max = kMaxMixerVolume;
    }
    int raw = min + (output_volume_ * (max - min) + 50) / 100;
    unsigned int count = mixer_ctl_get_num_values(ctl);
    for (unsigned int i = 0; i < count; ++i) {
        mixer_ctl_set_value(ctl, i, raw);
    }
    C1XZ_LOGI(TAG, "output volume %d%% (raw %d of %d..%d)", output_volume_, raw, min, max);
}
