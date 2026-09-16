// Host-build audio backend.
//
// Captures silence and discards playback, at real time pacing so the audio
// threads behave the way they would on the device. Used by the unit tests and
// for running the protocol against a server from a development machine.

#include <chrono>
#include <cstring>
#include <thread>

#include "audio/audio_codec.h"
#include "platform/log.h"

#define TAG "NullAudio"

namespace {

class NullAudioCodec : public AudioCodec {
public:
    NullAudioCodec() {
        input_sample_rate_ = 16000;
        input_channels_ = 1;
        output_sample_rate_ = 48000;
        output_channels_ = 1;
    }

    bool Start() override { return true; }
    void Stop() override {}

    void EnableInput(bool enable) override { input_enabled_ = enable; }
    void EnableOutput(bool enable) override { output_enabled_ = enable; }

    int Read(int16_t* dest, int samples) override {
        if (dest == nullptr || samples <= 0) {
            return -1;
        }
        memset(dest, 0, static_cast<size_t>(samples) * sizeof(int16_t));
        // Pace like real hardware would, otherwise the input thread spins.
        std::this_thread::sleep_for(
            std::chrono::milliseconds(samples * 1000 / input_sample_rate_));
        return samples;
    }

    int Write(const int16_t* data, int samples) override {
        (void)data;
        std::this_thread::sleep_for(
            std::chrono::milliseconds(samples * 1000 / output_sample_rate_));
        return samples;
    }

    void SetOutputVolume(int volume) override { output_volume_ = volume; }
};

}  // namespace

AudioCodec* CreateHostAudioCodec() {
    static NullAudioCodec codec;
    return &codec;
}
