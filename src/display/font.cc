#include "display/font.h"

#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "display/gfx.h"
#include "platform/log.h"

#define TAG "Font"

namespace c1xz {
namespace {

#include "font_ascii.inc"

constexpr char kMagic[8] = {'C', '1', 'F', 'O', 'N', 'T', '1', '\0'};

}  // namespace

Font::Font() = default;

Font::~Font() {
    if (mapped_ != nullptr) {
        munmap(mapped_, mapped_size_);
    }
}

bool Font::LoadFile(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        C1XZ_LOGW(TAG, "no font file at %s; falling back to the built-in ASCII set",
                  path.c_str());
        return false;
    }
    struct stat st {};
    if (fstat(fd, &st) != 0 || st.st_size < 16) {
        close(fd);
        return false;
    }
    void* base = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (base == MAP_FAILED) {
        C1XZ_LOGE(TAG, "could not map %s", path.c_str());
        return false;
    }

    const uint8_t* p = static_cast<const uint8_t*>(base);
    if (memcmp(p, kMagic, sizeof(kMagic)) != 0) {
        C1XZ_LOGE(TAG, "%s is not a C1FONT1 file", path.c_str());
        munmap(base, static_cast<size_t>(st.st_size));
        return false;
    }
    uint16_t height = static_cast<uint16_t>(p[8] | (p[9] << 8));
    uint32_t count = static_cast<uint32_t>(p[12]) | (static_cast<uint32_t>(p[13]) << 8) |
                     (static_cast<uint32_t>(p[14]) << 16) | (static_cast<uint32_t>(p[15]) << 24);

    size_t index_bytes = static_cast<size_t>(count) * sizeof(Index);
    size_t rows_bytes = static_cast<size_t>(count) * height * sizeof(uint16_t);
    if (16 + index_bytes + rows_bytes > static_cast<size_t>(st.st_size)) {
        C1XZ_LOGE(TAG, "%s is truncated: %u glyphs at %u rows need more than %lld bytes",
                  path.c_str(), count, height, static_cast<long long>(st.st_size));
        munmap(base, static_cast<size_t>(st.st_size));
        return false;
    }

    if (mapped_ != nullptr) {
        munmap(mapped_, mapped_size_);
    }
    mapped_ = base;
    mapped_size_ = static_cast<size_t>(st.st_size);
    height_ = height;
    count_ = count;
    index_ = reinterpret_cast<const Index*>(p + 16);
    rows_ = reinterpret_cast<const uint16_t*>(p + 16 + index_bytes);
    C1XZ_LOGI(TAG, "loaded %s: %u glyphs, %d rows tall", path.c_str(), count_, height_);
    return true;
}

long Font::Find(uint32_t codepoint) const {
    if (index_ == nullptr || count_ == 0) {
        return -1;
    }
    long low = 0;
    long high = static_cast<long>(count_) - 1;
    while (low <= high) {
        long mid = (low + high) / 2;
        uint32_t value = index_[mid].codepoint;
        if (value == codepoint) {
            return mid;
        }
        if (value < codepoint) {
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }
    return -1;
}

bool Font::GetGlyph(uint32_t codepoint, const uint16_t** rows, int* width,
                    uint16_t* scratch) const {
    long found = Find(codepoint);
    if (found >= 0) {
        *rows = rows_ + static_cast<size_t>(found) * height_;
        *width = index_[found].width;
        return true;
    }

    if (codepoint >= 0x20 && codepoint < 0x7F) {
        const unsigned char* ascii = kAsciiFont8x16[codepoint - 0x20];
        for (int i = 0; i < 16 && i < height_; ++i) {
            scratch[i] = static_cast<uint16_t>(ascii[i] << 8);
        }
        for (int i = 16; i < height_; ++i) {
            scratch[i] = 0;
        }
        *rows = scratch;
        *width = 8;
        return true;
    }

    // Unknown code point: draw a hollow box so the gap is visible rather than
    // silently swallowing text.
    int box_width = codepoint < 0x80 ? 8 : 16;
    uint16_t mask = box_width == 8 ? 0xFE00 : 0xFFFF;
    for (int i = 0; i < height_; ++i) {
        if (i < 2 || i >= height_ - 2) {
            scratch[i] = 0;
        } else if (i == 2 || i == height_ - 3) {
            scratch[i] = mask;
        } else {
            scratch[i] = static_cast<uint16_t>(mask & ~(mask >> 1) ) |
                         static_cast<uint16_t>(box_width == 8 ? 0x0200 : 0x0001);
        }
    }
    *rows = scratch;
    *width = box_width;
    return false;
}

int Font::GlyphWidth(uint32_t codepoint) const {
    long found = Find(codepoint);
    if (found >= 0) {
        return index_[found].width;
    }
    return codepoint < 0x80 ? 8 : 16;
}

int Font::MeasureUtf8(const std::string& text) const {
    int total = 0;
    size_t offset = 0;
    while (offset < text.size()) {
        total += GlyphWidth(Utf8Next(text, &offset));
    }
    return total;
}

}  // namespace c1xz
