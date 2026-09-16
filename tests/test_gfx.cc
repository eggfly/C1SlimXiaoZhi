#include "display/font.h"
#include "display/gfx.h"
#include "test_framework.h"

#include <string>

using c1xz::Canvas;
using c1xz::Font;
using c1xz::Utf8Next;

TEST(FrameLayoutMatchesTheDriver) {
    // The panel driver reads offset = (y>>3)*width + x with mask 0x80>>(y&7)
    // and treats a set bit as black. A mismatch here would show up on hardware
    // as a scrambled screen, which is expensive to discover.
    Canvas canvas(296, 152);
    CHECK_EQ(canvas.size(), static_cast<size_t>(5624));

    canvas.Clear(false);
    canvas.SetPixel(0, 0, true);
    CHECK_EQ(canvas.data()[0], 0x80);

    canvas.Clear(false);
    canvas.SetPixel(0, 7, true);
    CHECK_EQ(canvas.data()[0], 0x01);

    canvas.Clear(false);
    canvas.SetPixel(1, 8, true);
    CHECK_EQ(canvas.data()[296 + 1], 0x80);

    canvas.Clear(false);
    canvas.SetPixel(295, 151, true);
    CHECK_EQ(canvas.data()[(151 / 8) * 296 + 295], 0x01);
}

TEST(PixelsOutsideTheCanvasAreIgnored) {
    Canvas canvas(296, 152);
    canvas.Clear(false);
    canvas.SetPixel(-1, 0, true);
    canvas.SetPixel(0, -1, true);
    canvas.SetPixel(296, 0, true);
    canvas.SetPixel(0, 152, true);
    for (size_t i = 0; i < canvas.size(); ++i) {
        CHECK_EQ(canvas.data()[i], 0x00);
    }
}

TEST(ClearAndInvert) {
    Canvas canvas(16, 16);
    canvas.Clear(true);
    CHECK(canvas.GetPixel(0, 0));
    canvas.InvertRect(0, 0, 16, 16);
    CHECK(!canvas.GetPixel(0, 0));
}

TEST(Utf8Decoding) {
    std::string text = "a\xe4\xbd\xa0";  // 'a' then U+4F60
    size_t offset = 0;
    CHECK_EQ(Utf8Next(text, &offset), static_cast<uint32_t>('a'));
    CHECK_EQ(offset, static_cast<size_t>(1));
    CHECK_EQ(Utf8Next(text, &offset), static_cast<uint32_t>(0x4F60));
    CHECK_EQ(offset, text.size());
}

TEST(Utf8MalformedDoesNotDesynchronise) {
    std::string text = "\xff"
                       "ok";
    size_t offset = 0;
    CHECK_EQ(Utf8Next(text, &offset), static_cast<uint32_t>(0xFFFD));
    CHECK_EQ(Utf8Next(text, &offset), static_cast<uint32_t>('o'));
    CHECK_EQ(Utf8Next(text, &offset), static_cast<uint32_t>('k'));
    CHECK_EQ(offset, text.size());
}

TEST(AsciiFallbackDrawsWithoutAFontFile) {
    // No LoadFile call: the compiled-in ASCII set must still work, because the
    // activation code has to be readable even if the font file is missing.
    Font font;
    CHECK(!font.has_file());
    CHECK_EQ(font.GlyphWidth('A'), 8);
    CHECK_EQ(font.MeasureUtf8("ABC"), 24);

    Canvas canvas(296, 152);
    canvas.Clear(false);
    int end = canvas.DrawText(0, 0, "A", font, true);
    CHECK_EQ(end, 8);

    bool any_black = false;
    for (size_t i = 0; i < canvas.size(); ++i) {
        if (canvas.data()[i] != 0) {
            any_black = true;
            break;
        }
    }
    CHECK(any_black);
}

TEST(WrappingStaysInsideTheBox) {
    Font font;
    Canvas canvas(296, 152);
    canvas.Clear(false);
    std::string text =
        "the quick brown fox jumps over the lazy dog and keeps going for a while longer";
    int bottom = canvas.DrawTextWrapped(2, 20, 292, 152, text, font, true);
    CHECK(bottom > 20);
    // Nothing may be drawn in the two pixel gutter on the right.
    for (int y = 0; y < 152; ++y) {
        CHECK(!canvas.GetPixel(295, y));
    }
}

TEST(MeasureWrappedHeightGrowsWithText) {
    Font font;
    Canvas canvas(296, 152);
    int one = canvas.MeasureWrappedHeight(292, "short", font);
    int many = canvas.MeasureWrappedHeight(
        40, "a much longer string that certainly has to wrap several times", font);
    CHECK_EQ(one, font.height());
    CHECK(many > one);
}

TEST(EqualityDetectsIdenticalFrames) {
    // The e-paper layer skips submitting a frame identical to the last one.
    Canvas a(296, 152);
    Canvas b(296, 152);
    a.Clear(false);
    b.Clear(false);
    CHECK(a == b);
    a.SetPixel(10, 10, true);
    CHECK(a != b);
}
