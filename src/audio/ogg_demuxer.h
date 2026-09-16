#ifndef C1XZ_AUDIO_OGG_DEMUXER_H
#define C1XZ_AUDIO_OGG_DEMUXER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace c1xz {

// Minimal Ogg page parser, enough to pull the Opus packets out of the prompt
// sounds shipped with xiaozhi-esp32 (success.ogg, exclamation.ogg and the
// localised activation clips).
//
// Only what those files use is handled: a single logical stream, no chaining,
// no seeking. The OpusHead and OpusTags headers are recognised and skipped.
class OggDemuxer {
public:
    struct OpusHead {
        uint8_t channels = 1;
        uint16_t pre_skip = 0;
        uint32_t input_sample_rate = 48000;
        bool valid = false;
    };

    // Calls `on_packet` for every audio packet, in order. Returns false if the
    // data is not a well formed Ogg Opus stream.
    static bool Parse(std::string_view data, const OpusHead** head_out,
                      const std::function<bool(const uint8_t* data, size_t len)>& on_packet);

    // Convenience form that collects the packets into a vector.
    static bool ParseToPackets(std::string_view data, OpusHead* head,
                               std::vector<std::vector<uint8_t>>* packets);
};

}  // namespace c1xz

#endif  // C1XZ_AUDIO_OGG_DEMUXER_H
