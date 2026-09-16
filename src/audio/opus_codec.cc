#include "audio/opus_codec.h"

#include <opus.h>

#include "platform/log.h"

#define TAG "Opus"

namespace c1xz {

OpusEncoderWrapper::OpusEncoderWrapper() = default;

OpusEncoderWrapper::~OpusEncoderWrapper() {
    if (encoder_ != nullptr) {
        opus_encoder_destroy(encoder_);
    }
}

bool OpusEncoderWrapper::Configure(int sample_rate, int channels, int duration_ms) {
    if (encoder_ != nullptr) {
        opus_encoder_destroy(encoder_);
        encoder_ = nullptr;
    }
    int error = 0;
    // VOIP mode: the uplink is speech, and it costs less than the generic
    // AUDIO mode at the same bitrate.
    encoder_ = opus_encoder_create(sample_rate, channels, OPUS_APPLICATION_VOIP, &error);
    if (encoder_ == nullptr || error != OPUS_OK) {
        C1XZ_LOGE(TAG, "encoder create failed: %s", opus_strerror(error));
        encoder_ = nullptr;
        return false;
    }
    sample_rate_ = sample_rate;
    channels_ = channels;
    duration_ms_ = duration_ms;
    frame_size_ = sample_rate / 1000 * duration_ms;
    buffer_.resize(4000);  // well over the largest Opus packet at these rates

    SetComplexity(3);
    opus_encoder_ctl(encoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(encoder_, OPUS_SET_VBR(1));
    // Discontinuous transmission: silence costs almost no bandwidth. Upstream
    // enables it too.
    opus_encoder_ctl(encoder_, OPUS_SET_DTX(1));
    C1XZ_LOGI(TAG, "encoder ready: %d Hz, %d ch, %d ms (%d samples/frame)", sample_rate, channels,
              duration_ms, frame_size_);
    return true;
}

void OpusEncoderWrapper::SetComplexity(int complexity) {
    if (encoder_ != nullptr) {
        opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(complexity));
    }
}

void OpusEncoderWrapper::SetBitrate(int bits_per_second) {
    if (encoder_ != nullptr) {
        opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(bits_per_second));
    }
}

void OpusEncoderWrapper::SetDtx(bool enable) {
    if (encoder_ != nullptr) {
        opus_encoder_ctl(encoder_, OPUS_SET_DTX(enable ? 1 : 0));
    }
}

bool OpusEncoderWrapper::Encode(const int16_t* pcm, std::vector<uint8_t>* out) {
    if (encoder_ == nullptr || pcm == nullptr || out == nullptr) {
        return false;
    }
    int written = opus_encode(encoder_, pcm, frame_size_, buffer_.data(),
                              static_cast<opus_int32>(buffer_.size()));
    if (written < 0) {
        C1XZ_LOGE(TAG, "encode failed: %s", opus_strerror(written));
        return false;
    }
    out->assign(buffer_.begin(), buffer_.begin() + written);
    return true;
}

OpusDecoderWrapper::OpusDecoderWrapper() = default;

OpusDecoderWrapper::~OpusDecoderWrapper() {
    if (decoder_ != nullptr) {
        opus_decoder_destroy(decoder_);
    }
}

bool OpusDecoderWrapper::Configure(int sample_rate, int channels, int duration_ms) {
    if (decoder_ != nullptr) {
        opus_decoder_destroy(decoder_);
        decoder_ = nullptr;
    }
    int error = 0;
    decoder_ = opus_decoder_create(sample_rate, channels, &error);
    if (decoder_ == nullptr || error != OPUS_OK) {
        C1XZ_LOGE(TAG, "decoder create failed: %s", opus_strerror(error));
        decoder_ = nullptr;
        return false;
    }
    sample_rate_ = sample_rate;
    channels_ = channels;
    duration_ms_ = duration_ms;
    frame_size_ = sample_rate / 1000 * duration_ms;
    C1XZ_LOGI(TAG, "decoder ready: %d Hz, %d ch, %d ms (%d samples/frame)", sample_rate, channels,
              duration_ms, frame_size_);
    return true;
}

bool OpusDecoderWrapper::Decode(const uint8_t* data, size_t length, std::vector<int16_t>* pcm) {
    if (decoder_ == nullptr || pcm == nullptr) {
        return false;
    }
    pcm->resize(static_cast<size_t>(frame_size_) * channels_);
    int samples = opus_decode(decoder_, data, static_cast<opus_int32>(length), pcm->data(),
                              frame_size_, 0);
    if (samples < 0) {
        C1XZ_LOGE(TAG, "decode failed: %s", opus_strerror(samples));
        return false;
    }
    pcm->resize(static_cast<size_t>(samples) * channels_);
    return true;
}

bool OpusDecoderWrapper::DecodeLost(std::vector<int16_t>* pcm) {
    if (decoder_ == nullptr || pcm == nullptr) {
        return false;
    }
    pcm->resize(static_cast<size_t>(frame_size_) * channels_);
    int samples = opus_decode(decoder_, nullptr, 0, pcm->data(), frame_size_, 0);
    if (samples < 0) {
        return false;
    }
    pcm->resize(static_cast<size_t>(samples) * channels_);
    return true;
}

void OpusDecoderWrapper::Reset() {
    if (decoder_ != nullptr) {
        opus_decoder_ctl(decoder_, OPUS_RESET_STATE);
    }
}

}  // namespace c1xz
