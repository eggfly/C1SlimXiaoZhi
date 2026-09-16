#include "audio/ogg_demuxer.h"

#include <cstring>

#include "platform/log.h"

#define TAG "Ogg"

namespace c1xz {
namespace {

uint32_t ReadLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t ReadLe16(const uint8_t* p) {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}

thread_local OggDemuxer::OpusHead t_head;

}  // namespace

bool OggDemuxer::Parse(std::string_view data, const OpusHead** head_out,
                       const std::function<bool(const uint8_t* data, size_t len)>& on_packet) {
    t_head = OpusHead();
    if (head_out != nullptr) {
        *head_out = &t_head;
    }

    const uint8_t* p = reinterpret_cast<const uint8_t*>(data.data());
    size_t size = data.size();
    size_t offset = 0;
    std::vector<uint8_t> pending;  // packet spanning several segments or pages
    int packet_index = 0;

    while (offset + 27 <= size) {
        if (memcmp(p + offset, "OggS", 4) != 0) {
            C1XZ_LOGE(TAG, "lost page sync at offset %zu", offset);
            return false;
        }
        uint8_t page_segments = p[offset + 26];
        size_t header_size = 27 + page_segments;
        if (offset + header_size > size) {
            C1XZ_LOGE(TAG, "truncated page header");
            return false;
        }
        const uint8_t* segment_table = p + offset + 27;
        size_t body_size = 0;
        for (uint8_t i = 0; i < page_segments; ++i) {
            body_size += segment_table[i];
        }
        if (offset + header_size + body_size > size) {
            C1XZ_LOGE(TAG, "truncated page body");
            return false;
        }

        const uint8_t* body = p + offset + header_size;
        size_t body_offset = 0;
        for (uint8_t i = 0; i < page_segments; ++i) {
            uint8_t segment_len = segment_table[i];
            pending.insert(pending.end(), body + body_offset, body + body_offset + segment_len);
            body_offset += segment_len;
            if (segment_len == 255) {
                continue;  // packet continues into the next segment
            }

            // A complete packet.
            if (packet_index == 0) {
                if (pending.size() >= 19 && memcmp(pending.data(), "OpusHead", 8) == 0) {
                    t_head.channels = pending[9];
                    t_head.pre_skip = ReadLe16(pending.data() + 10);
                    t_head.input_sample_rate = ReadLe32(pending.data() + 12);
                    t_head.valid = true;
                } else {
                    C1XZ_LOGE(TAG, "first packet is not OpusHead");
                    return false;
                }
            } else if (packet_index == 1 && pending.size() >= 8 &&
                       memcmp(pending.data(), "OpusTags", 8) == 0) {
                // Metadata, nothing to do.
            } else if (!pending.empty()) {
                if (!on_packet(pending.data(), pending.size())) {
                    return true;  // consumer asked to stop
                }
            }
            ++packet_index;
            pending.clear();
        }
        offset += header_size + body_size;
    }

    if (!t_head.valid) {
        C1XZ_LOGE(TAG, "no OpusHead found");
        return false;
    }
    return true;
}

bool OggDemuxer::ParseToPackets(std::string_view data, OpusHead* head,
                                std::vector<std::vector<uint8_t>>* packets) {
    const OpusHead* parsed = nullptr;
    bool ok = Parse(data, &parsed, [packets](const uint8_t* bytes, size_t len) {
        packets->emplace_back(bytes, bytes + len);
        return true;
    });
    if (ok && head != nullptr && parsed != nullptr) {
        *head = *parsed;
    }
    return ok;
}

}  // namespace c1xz
