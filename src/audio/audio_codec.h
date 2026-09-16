#ifndef C1XZ_AUDIO_AUDIO_CODEC_H
#define C1XZ_AUDIO_AUDIO_CODEC_H

#include <cstdint>
#include <string>
#include <vector>

// Hardware audio interface, in the shape of the upstream AudioCodec class.
//
// On the C1 Slim this is ALSA card 0 device 0 driven through tinyalsa: an
// ES8326 codec that captures 16 kHz mono and plays back at the rate the driver
// prefers. See docs/MICROPHONE.md for how those parameters were established.
class AudioCodec {
public:
    virtual ~AudioCodec() = default;

    virtual bool Start() = 0;
    virtual void Stop() = 0;

    // Enables or disables the capture and playback paths. Closing them when
    // idle lets the codec power down, which matters on battery.
    virtual void EnableInput(bool enable) = 0;
    virtual void EnableOutput(bool enable) = 0;

    // Blocking. Returns the number of samples read, or a negative value on an
    // unrecoverable error. Fills exactly `samples` interleaved mono samples.
    virtual int Read(int16_t* dest, int samples) = 0;

    // Blocking. Writes interleaved samples at output_sample_rate().
    virtual int Write(const int16_t* data, int samples) = 0;

    // 0-100. Backed by the ES8326 "DAC Playback Volume" mixer control.
    virtual void SetOutputVolume(int volume) = 0;
    virtual int output_volume() const { return output_volume_; }

    virtual int input_sample_rate() const { return input_sample_rate_; }
    virtual int output_sample_rate() const { return output_sample_rate_; }
    virtual int input_channels() const { return input_channels_; }
    virtual int output_channels() const { return output_channels_; }

    virtual bool input_enabled() const { return input_enabled_; }
    virtual bool output_enabled() const { return output_enabled_; }

    // True when this codec can capture and play at the same time. The C1 Slim
    // can, but without echo cancellation the microphone hears the speaker, so
    // the application still runs half duplex unless AEC is enabled.
    virtual bool duplex() const { return true; }

protected:
    int input_sample_rate_ = 16000;
    int output_sample_rate_ = 16000;
    int input_channels_ = 1;
    int output_channels_ = 1;
    int output_volume_ = 70;
    bool input_enabled_ = false;
    bool output_enabled_ = false;
};

#endif  // C1XZ_AUDIO_AUDIO_CODEC_H
