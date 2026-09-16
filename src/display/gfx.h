#ifndef C1XZ_DISPLAY_GFX_H
#define C1XZ_DISPLAY_GFX_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace c1xz {

class Font;

// One-bit canvas stored in the exact byte order the C1 Slim's e-paper driver
// expects, so a full frame is a single write() to /dev/epaper_lcd with no
// repacking:
//
//   offset = (y >> 3) * width + x,  mask = 0x80 >> (y & 7),  black = 1
//
// For the 296x152 panel that is 19 * 296 = 5624 bytes.
class Canvas {
public:
    Canvas(int width, int height);

    void Clear(bool black = false);
    void SetPixel(int x, int y, bool black);
    bool GetPixel(int x, int y) const;

    void FillRect(int x, int y, int w, int h, bool black);
    void DrawRect(int x, int y, int w, int h, bool black);
    void DrawHLine(int x, int y, int length, bool black);
    void DrawVLine(int x, int y, int length, bool black);
    void InvertRect(int x, int y, int w, int h);

    // Draws UTF-8 text with the top-left corner at (x, y). Returns the x
    // coordinate just past the last glyph. Characters missing from the font are
    // drawn as a hollow box rather than skipped, so missing coverage is visible
    // instead of silently dropping text.
    int DrawText(int x, int y, const std::string& utf8, const Font& font, bool black = true);

    // Word-wraps into a box and returns the y coordinate below the last line.
    // Breaks between CJK characters and at spaces for Latin text.
    int DrawTextWrapped(int x, int y, int max_width, int max_y, const std::string& utf8,
                        const Font& font, bool black = true);

    // Height the same text would occupy, without drawing it.
    int MeasureWrappedHeight(int max_width, const std::string& utf8, const Font& font) const;

    int width() const { return width_; }
    int height() const { return height_; }

    const uint8_t* data() const { return buffer_.data(); }
    size_t size() const { return buffer_.size(); }

    bool operator==(const Canvas& other) const { return buffer_ == other.buffer_; }
    bool operator!=(const Canvas& other) const { return buffer_ != other.buffer_; }

private:
    int width_;
    int height_;
    std::vector<uint8_t> buffer_;
};

// Decodes one UTF-8 code point. Advances *offset. Returns U+FFFD on malformed
// input so a bad byte cannot desynchronise the rest of the string.
uint32_t Utf8Next(const std::string& text, size_t* offset);

}  // namespace c1xz

#endif  // C1XZ_DISPLAY_GFX_H
