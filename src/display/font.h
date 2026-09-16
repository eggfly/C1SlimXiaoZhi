#ifndef C1XZ_DISPLAY_FONT_H
#define C1XZ_DISPLAY_FONT_H

#include <cstdint>
#include <string>

namespace c1xz {

// Bitmap font in the C1FONT1 format produced by tools/build_font.py from GNU
// Unifont. Glyphs are 16 rows tall and either 8 or 16 pixels wide, stored as
// one 16-bit word per row with the leftmost pixel in the high bit.
//
// The file is memory mapped, so the ~900 KiB CJK set costs almost no resident
// memory: only the pages holding glyphs actually drawn are faulted in, which
// matters on a device with about 50 MiB of RAM.
//
// A compiled-in ASCII subset is always available, so the application can render
// an activation code or an error even when the font file is missing.
class Font {
public:
    Font();
    ~Font();

    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;

    // Loads a C1FONT1 file. Returns false and keeps the ASCII fallback if the
    // file is missing or malformed.
    bool LoadFile(const std::string& path);
    bool has_file() const { return mapped_ != nullptr; }

    int height() const { return height_; }

    // Returns the glyph's rows and width. Falls back to the built-in ASCII set,
    // then to a hollow box for anything unavailable.
    bool GetGlyph(uint32_t codepoint, const uint16_t** rows, int* width,
                  uint16_t* scratch) const;

    // Advance width in pixels for a code point.
    int GlyphWidth(uint32_t codepoint) const;

    // Pixel width of a UTF-8 string.
    int MeasureUtf8(const std::string& text) const;

private:
    struct Index {
        uint32_t codepoint;
        uint8_t width;
        uint8_t pad[3];
    };

    void* mapped_ = nullptr;
    size_t mapped_size_ = 0;
    const Index* index_ = nullptr;
    const uint16_t* rows_ = nullptr;
    uint32_t count_ = 0;
    int height_ = 16;

    long Find(uint32_t codepoint) const;
};

}  // namespace c1xz

#endif  // C1XZ_DISPLAY_FONT_H
