#ifndef C1XZ_AUDIO_OPUS_CODEC_H
#define C1XZ_AUDIO_OPUS_CODEC_H

#include <cstdint>
#include <memory>
#include <vector>

struct OpusEncoder;
struct OpusDecoder;

namespace c1xz {

// Thin libopus wrappers.
//
// The device already ships libopus 1.4 and the factory recorder encodes Opus on
// this same CPU, so the cost here is known to be affordable. We build the
// fixed-point flavour, which is the one with MIPS-specific optimisations.
class OpusEncoderWrapper {
public:
    OpusEncoderWrapper();
    ~OpusEncoderWrapper();

    OpusEncoderWrapper(const OpusEncoderWrapper&) = delete;
    OpusEncoderWrapper& operator=(const OpusEncoderWrapper&) = delete;

    // `duration_ms` must be one of Opus's frame sizes: 10, 20, 40, 60...
    bool Configure(int sample_rate, int channels, int duration_ms);

    // Complexity trades CPU for quality: 0 is cheapest, 10 is best. The default
    // is deliberately low because this is a 1 GHz single core that is also
    // driving a 685 ms-per-frame display.
    void SetComplexity(int complexity);
    void SetBitrate(int bits_per_second);
    void SetDtx(bool enable);

    // Encodes exactly frame_size() samples. Returns false on error.
    bool Encode(const int16_t* pcm, std::vector<uint8_t>* out);

    int frame_size() const { return frame_size_; }
    int sample_rate() const { return sample_rate_; }
    int duration_ms() const { return duration_ms_; }

private:
    OpusEncoder* encoder_ = nullptr;
    int sample_rate_ = 16000;
    int channels_ = 1;
    int duration_ms_ = 60;
    int frame_size_ = 0;
    std::vector<uint8_t> buffer_;
};

class OpusDecoderWrapper {
public:
    OpusDecoderWrapper();
    ~OpusDecoderWrapper();

    OpusDecoderWrapper(const OpusDecoderWrapper&) = delete;
    OpusDecoderWrapper& operator=(const OpusDecoderWrapper&) = delete;

    // Creating the decoder at the hardware's playback rate makes libopus do the
    // rate conversion internally, so no separate resampler is needed even when
    // the server streams 24 kHz.
    bool Configure(int sample_rate, int channels, int duration_ms);

    bool Decode(const uint8_t* data, size_t length, std::vector<int16_t>* pcm);

    // Produces one frame of concealment audio for a lost packet.
    bool DecodeLost(std::vector<int16_t>* pcm);

    void Reset();

    int frame_size() const { return frame_size_; }
    int sample_rate() const { return sample_rate_; }

private:
    OpusDecoder* decoder_ = nullptr;
    int sample_rate_ = 24000;
    int channels_ = 1;
    int duration_ms_ = 60;
    int frame_size_ = 0;
};

}  // namespace c1xz

#endif  // C1XZ_AUDIO_OPUS_CODEC_H
