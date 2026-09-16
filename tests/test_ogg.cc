#include "audio/ogg_demuxer.h"
#include "test_framework.h"

#include <cstring>
#include <string>
#include <vector>

using c1xz::OggDemuxer;

namespace {

// Builds one Ogg page around the given packets. Enough structure for the
// demuxer; the CRC is not checked by it, so it is left zero.
std::string MakePage(const std::vector<std::string>& packets, bool first_page) {
    std::string segments;
    std::string body;
    for (const auto& packet : packets) {
        size_t remaining = packet.size();
        while (remaining >= 255) {
            segments.push_back(static_cast<char>(255));
            remaining -= 255;
        }
        segments.push_back(static_cast<char>(remaining));
        body += packet;
    }

    std::string page = "OggS";
    page.push_back(0);                                     // version
    page.push_back(first_page ? 0x02 : 0x00);              // header type
    page.append(8, '\0');                                  // granule position
    page.append(4, '\0');                                  // serial
    page.append(4, '\0');                                  // sequence
    page.append(4, '\0');                                  // crc
    page.push_back(static_cast<char>(segments.size()));    // segment count
    page += segments;
    page += body;
    return page;
}

std::string MakeOpusHead(uint8_t channels, uint16_t pre_skip) {
    std::string head = "OpusHead";
    head.push_back(1);                                        // version
    head.push_back(static_cast<char>(channels));
    head.push_back(static_cast<char>(pre_skip & 0xff));
    head.push_back(static_cast<char>((pre_skip >> 8) & 0xff));
    head.push_back(static_cast<char>(0x80));                  // 48000 Hz, LE
    head.push_back(static_cast<char>(0xbb));
    head.push_back(0);
    head.push_back(0);
    head.append(2, '\0');                                     // output gain
    head.push_back(0);                                        // mapping family
    return head;
}

}  // namespace

TEST(ReadsHeaderAndPackets) {
    std::string stream = MakePage({MakeOpusHead(1, 312)}, true);
    stream += MakePage({std::string("OpusTags") + std::string(8, '\0')}, false);
    stream += MakePage({std::string(40, 'A'), std::string(37, 'B')}, false);

    OggDemuxer::OpusHead head;
    std::vector<std::vector<uint8_t>> packets;
    CHECK(OggDemuxer::ParseToPackets(stream, &head, &packets));
    CHECK(head.valid);
    CHECK_EQ(static_cast<int>(head.channels), 1);
    CHECK_EQ(static_cast<int>(head.pre_skip), 312);
    CHECK_EQ(head.input_sample_rate, static_cast<uint32_t>(48000));
    CHECK_EQ(packets.size(), static_cast<size_t>(2));
    CHECK_EQ(packets[0].size(), static_cast<size_t>(40));
    CHECK_EQ(packets[1].size(), static_cast<size_t>(37));
    CHECK_EQ(packets[0][0], static_cast<uint8_t>('A'));
}

TEST(HandlesAPacketSpanningSegments) {
    // A packet over 255 bytes is split across segments, terminated by one
    // shorter than 255.
    std::string big(600, 'X');
    std::string stream = MakePage({MakeOpusHead(1, 0)}, true);
    stream += MakePage({std::string("OpusTags") + std::string(8, '\0')}, false);
    stream += MakePage({big}, false);

    OggDemuxer::OpusHead head;
    std::vector<std::vector<uint8_t>> packets;
    CHECK(OggDemuxer::ParseToPackets(stream, &head, &packets));
    CHECK_EQ(packets.size(), static_cast<size_t>(1));
    CHECK_EQ(packets[0].size(), static_cast<size_t>(600));
}

TEST(RejectsAStreamWithoutOpusHead) {
    std::string stream = MakePage({std::string("NotOpus!") + std::string(12, '\0')}, true);
    OggDemuxer::OpusHead head;
    std::vector<std::vector<uint8_t>> packets;
    CHECK(!OggDemuxer::ParseToPackets(stream, &head, &packets));
}

TEST(RejectsTruncatedInput) {
    std::string stream = MakePage({MakeOpusHead(1, 0)}, true);
    stream.resize(stream.size() - 5);
    OggDemuxer::OpusHead head;
    std::vector<std::vector<uint8_t>> packets;
    CHECK(!OggDemuxer::ParseToPackets(stream, &head, &packets));
}

TEST(RejectsGarbage) {
    // Prompt sounds are files on a writable partition, so a corrupt one must
    // not take the process down.
    std::string garbage(512, '\x7f');
    OggDemuxer::OpusHead head;
    std::vector<std::vector<uint8_t>> packets;
    CHECK(!OggDemuxer::ParseToPackets(garbage, &head, &packets));
}
