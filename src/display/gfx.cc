#include "display/gfx.h"

#include <algorithm>
#include <cstring>

#include "display/font.h"

namespace c1xz {

uint32_t Utf8Next(const std::string& text, size_t* offset) {
    size_t i = *offset;
    if (i >= text.size()) {
        *offset = text.size();
        return 0;
    }
    unsigned char first = static_cast<unsigned char>(text[i]);
    size_t extra;
    uint32_t value;
    if (first < 0x80) {
        *offset = i + 1;
        return first;
    } else if ((first & 0xE0) == 0xC0) {
        extra = 1;
        value = first & 0x1F;
    } else if ((first & 0xF0) == 0xE0) {
        extra = 2;
        value = first & 0x0F;
    } else if ((first & 0xF8) == 0xF0) {
        extra = 3;
        value = first & 0x07;
    } else {
        *offset = i + 1;
        return 0xFFFD;
    }
    if (i + extra >= text.size()) {
        *offset = text.size();
        return 0xFFFD;
    }
    for (size_t k = 1; k <= extra; ++k) {
        unsigned char c = static_cast<unsigned char>(text[i + k]);
        if ((c & 0xC0) != 0x80) {
            *offset = i + k;
            return 0xFFFD;
        }
        value = (value << 6) | (c & 0x3F);
    }
    *offset = i + extra + 1;
    return value;
}

Canvas::Canvas(int width, int height) : width_(width), height_(height) {
    buffer_.assign(static_cast<size_t>((height + 7) / 8) * width, 0);
}

void Canvas::Clear(bool black) {
    std::fill(buffer_.begin(), buffer_.end(), black ? 0xFF : 0x00);
}

void Canvas::SetPixel(int x, int y, bool black) {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) {
        return;
    }
    size_t offset = static_cast<size_t>(y >> 3) * width_ + x;
    uint8_t mask = static_cast<uint8_t>(0x80 >> (y & 7));
    if (black) {
        buffer_[offset] |= mask;
    } else {
        buffer_[offset] &= static_cast<uint8_t>(~mask);
    }
}

bool Canvas::GetPixel(int x, int y) const {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) {
        return false;
    }
    size_t offset = static_cast<size_t>(y >> 3) * width_ + x;
    uint8_t mask = static_cast<uint8_t>(0x80 >> (y & 7));
    return (buffer_[offset] & mask) != 0;
}

void Canvas::FillRect(int x, int y, int w, int h, bool black) {
    for (int row = y; row < y + h; ++row) {
        for (int col = x; col < x + w; ++col) {
            SetPixel(col, row, black);
        }
    }
}

void Canvas::DrawRect(int x, int y, int w, int h, bool black) {
    if (w <= 0 || h <= 0) {
        return;
    }
    DrawHLine(x, y, w, black);
    DrawHLine(x, y + h - 1, w, black);
    DrawVLine(x, y, h, black);
    DrawVLine(x + w - 1, y, h, black);
}

void Canvas::DrawHLine(int x, int y, int length, bool black) {
    for (int i = 0; i < length; ++i) {
        SetPixel(x + i, y, black);
    }
}

void Canvas::DrawVLine(int x, int y, int length, bool black) {
    for (int i = 0; i < length; ++i) {
        SetPixel(x, y + i, black);
    }
}

void Canvas::InvertRect(int x, int y, int w, int h) {
    for (int row = y; row < y + h; ++row) {
        for (int col = x; col < x + w; ++col) {
            SetPixel(col, row, !GetPixel(col, row));
        }
    }
}

int Canvas::DrawText(int x, int y, const std::string& utf8, const Font& font, bool black) {
    uint16_t scratch[64];
    size_t offset = 0;
    int pen = x;
    while (offset < utf8.size()) {
        uint32_t codepoint = Utf8Next(utf8, &offset);
        if (codepoint == '\n') {
            break;
        }
        const uint16_t* rows = nullptr;
        int glyph_width = 0;
        font.GetGlyph(codepoint, &rows, &glyph_width, scratch);
        if (pen >= width_) {
            break;
        }
        for (int row = 0; row < font.height(); ++row) {
            uint16_t bits = rows[row];
            if (bits == 0) {
                continue;
            }
            for (int col = 0; col < glyph_width; ++col) {
                if (bits & (0x8000 >> col)) {
                    SetPixel(pen + col, y + row, black);
                }
            }
        }
        pen += glyph_width;
    }
    return pen;
}

namespace {

// CJK, kana and full-width punctuation can break between any two characters;
// Latin script breaks at spaces.
bool BreakableBefore(uint32_t codepoint) {
    return codepoint >= 0x2E80;
}

// Closing punctuation should not start a line.
bool NoBreakBefore(uint32_t codepoint) {
    switch (codepoint) {
        case 0x3001:  // 、
        case 0x3002:  // 。
        case 0xFF0C:  // ，
        case 0xFF0E:  // ．
        case 0xFF1A:  // ：
        case 0xFF1B:  // ；
        case 0xFF01:  // ！
        case 0xFF1F:  // ？
        case 0xFF09:  // ）
        case 0x300D:  // 」
        case 0x300F:  // 』
            return true;
        default:
            return false;
    }
}

}  // namespace

int Canvas::DrawTextWrapped(int x, int y, int max_width, int max_y, const std::string& utf8,
                            const Font& font, bool black) {
    const int line_height = font.height();
    int pen_y = y;
    size_t offset = 0;
    std::string line;
    int line_width = 0;
    size_t last_break = std::string::npos;
    int width_at_break = 0;

    auto flush = [&]() {
        if (!line.empty() && pen_y + line_height <= max_y) {
            DrawText(x, pen_y, line, font, black);
        }
        pen_y += line_height;
        line.clear();
        line_width = 0;
        last_break = std::string::npos;
    };

    while (offset < utf8.size()) {
        size_t start = offset;
        uint32_t codepoint = Utf8Next(utf8, &offset);
        if (codepoint == '\n') {
            flush();
            continue;
        }
        int glyph_width = font.GlyphWidth(codepoint);
        std::string piece = utf8.substr(start, offset - start);

        if (line_width + glyph_width > max_width && !line.empty()) {
            if (last_break != std::string::npos && !BreakableBefore(codepoint)) {
                // Rewind to the last space so a Latin word is not split.
                std::string remainder = line.substr(last_break);
                line.erase(last_break);
                line_width = width_at_break;
                flush();
                line = remainder;
                line_width = font.MeasureUtf8(remainder);
            } else if (NoBreakBefore(codepoint)) {
                // Let the punctuation overhang rather than starting a line
                // with it.
            } else {
                flush();
            }
        }

        if (codepoint == ' ') {
            last_break = line.size() + piece.size();
            width_at_break = line_width + glyph_width;
        }
        line += piece;
        line_width += glyph_width;

        if (pen_y >= max_y) {
            break;
        }
    }
    if (!line.empty()) {
        flush();
    }
    return pen_y;
}

int Canvas::MeasureWrappedHeight(int max_width, const std::string& utf8, const Font& font) const {
    const int line_height = font.height();
    int lines = 1;
    int line_width = 0;
    size_t offset = 0;
    while (offset < utf8.size()) {
        uint32_t codepoint = Utf8Next(utf8, &offset);
        if (codepoint == '\n') {
            ++lines;
            line_width = 0;
            continue;
        }
        int glyph_width = font.GlyphWidth(codepoint);
        if (line_width + glyph_width > max_width && line_width > 0) {
            ++lines;
            line_width = 0;
        }
        line_width += glyph_width;
    }
    return lines * line_height;
}

}  // namespace c1xz
