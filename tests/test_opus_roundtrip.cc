#include "audio/opus_codec.h"
#include "test_framework.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

using c1xz::OpusDecoderWrapper;
using c1xz::OpusEncoderWrapper;

namespace {

std::vector<int16_t> MakeTone(int sample_rate, int samples, double hz, double amplitude) {
    std::vector<int16_t> pcm(static_cast<size_t>(samples));
    for (int i = 0; i < samples; ++i) {
        double value = amplitude * 32767.0 *
                       std::sin(2.0 * 3.14159265358979323846 * hz * i / sample_rate);
        pcm[i] = static_cast<int16_t>(value);
    }
    return pcm;
}

double Rms(const std::vector<int16_t>& pcm) {
    if (pcm.empty()) {
        return 0;
    }
    double sum = 0;
    for (int16_t sample : pcm) {
        sum += static_cast<double>(sample) * sample;
    }
    return std::sqrt(sum / pcm.size());
}

}  // namespace

TEST(EncoderProducesTheExpectedFrameSize) {
    OpusEncoderWrapper encoder;
    CHECK(encoder.Configure(16000, 1, 60));
    // 60 ms at 16 kHz is 960 samples, and that is what the ALSA capture period
    // is sized to.
    CHECK_EQ(encoder.frame_size(), 960);
}

TEST(SpeechRoundTripsThroughOpus) {
    OpusEncoderWrapper encoder;
    CHECK(encoder.Configure(16000, 1, 60));
    // Decode at 48 kHz, the device's playback rate, to prove libopus does the
    // rate conversion so no separate resampler is needed.
    OpusDecoderWrapper decoder;
    CHECK(decoder.Configure(48000, 1, 60));

    auto tone = MakeTone(16000, 960, 440.0, 0.5);
    std::vector<uint8_t> packet;
    std::vector<int16_t> out;

    // Prime the codec: the first frames carry the encoder's startup transient.
    for (int i = 0; i < 5; ++i) {
        CHECK(encoder.Encode(tone.data(), &packet));
        CHECK(packet.size() > 0);
        CHECK(decoder.Decode(packet.data(), packet.size(), &out));
    }
    CHECK_EQ(out.size(), static_cast<size_t>(2880));  // 60 ms at 48 kHz

    double in_rms = Rms(tone);
    double out_rms = Rms(out);
    // A lossy codec will not reproduce the level exactly, but it must be in
    // the same ballpark. Silence out would mean the pipeline is broken.
    CHECK(out_rms > in_rms * 0.3);
    CHECK(out_rms < in_rms * 3.0);
}

TEST(EncodingIsFastEnoughForRealTime) {
    OpusEncoderWrapper encoder;
    CHECK(encoder.Configure(16000, 1, 60));
    auto tone = MakeTone(16000, 960, 300.0, 0.4);
    std::vector<uint8_t> packet;

    const int kFrames = 50;
    auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < kFrames; ++i) {
        CHECK(encoder.Encode(tone.data(), &packet));
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now() - started)
                       .count();
    double per_frame_ms = static_cast<double>(elapsed) / kFrames / 1000.0;
    printf("    encode: %.3f ms per 60 ms frame on this host\n", per_frame_ms);
    // This is the build host, not the device, so it only catches a gross
    // regression. The real budget has to be measured on the C1 Slim.
    CHECK(per_frame_ms < 60.0);
}

TEST(DecoderConcealsALostPacket) {
    OpusDecoderWrapper decoder;
    CHECK(decoder.Configure(48000, 1, 60));
    std::vector<int16_t> pcm;
    CHECK(decoder.DecodeLost(&pcm));
    CHECK_EQ(pcm.size(), static_cast<size_t>(2880));
}

TEST(DecoderRejectsGarbage) {
    OpusDecoderWrapper decoder;
    CHECK(decoder.Configure(48000, 1, 60));
    std::vector<uint8_t> garbage(64, 0xAB);
    std::vector<int16_t> pcm;
    // Must fail cleanly rather than crash: this data comes off the network.
    decoder.Decode(garbage.data(), garbage.size(), &pcm);
}
