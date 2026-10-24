#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace knockthru {

struct Color {
    std::uint8_t r, g, b, a = 255;
};

struct Point {
    float x, y;
};

struct Rect {
    float x, y, w, h;
    bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

enum class Align { Left, Center, Right };

// Text rendering from one TrueType font file, through stb_truetype.
class Font {
public:
    explicit Font(const std::filesystem::path& path);
    ~Font();
    Font(const Font&) = delete;

    struct Glyph {
        int width = 0, height = 0, x_offset = 0, y_offset = 0;
        float advance = 0;
        std::vector<std::uint8_t> coverage;
    };
    const Glyph& glyph(char32_t codepoint, float size);
    float kerning(char32_t left, char32_t right, float size) const;
    float ascent(float size) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::map<std::pair<char32_t, int>, Glyph> cache_;
};

// An RGBA image with anti-aliased shapes and text. Row 0 is the top.
class Canvas {
public:
    Canvas(int width, int height);

    int width() const { return width_; }
    int height() const { return height_; }
    const std::vector<std::uint8_t>& pixels() const { return pixels_; }

    void clear(Color color);
    void rounded_rect(Rect rect, float radius, Color color);
    void circle(float cx, float cy, float radius, Color color);
    // Fills closed rings with the even-odd rule, so a ring inside another cuts a hole.
    void polygon(const std::vector<std::vector<Point>>& rings, Color color);
    // A ring between two radii, limited to angles from `from` to `to` (radians,
    // 0 points right, counterclockwise on screen).
    void arc(float cx, float cy, float inner, float outer, float from, float to, Color color);
    // Draws one line of text. `y` is the vertical center of the line.
    void text(Font& font, const std::string& utf8, float x, float y, float size, Color color,
        Align align = Align::Left);
    float measure(Font& font, const std::string& utf8, float size);
    // Shrinks `utf8` with a trailing "..." until it fits in `width`.
    std::string fit(Font& font, const std::string& utf8, float size, float width);

    // Splits `utf8` at spaces into lines no wider than `width`. The last of
    // `max_lines` lines is shortened with "..." if the text does not fit.
    std::vector<std::string> wrap(
        Font& font, const std::string& utf8, float size, float width, int max_lines);

    // Blends another canvas onto this one with its top left corner at (x, y),
    // with its opacity scaled by `alpha`.
    void draw(const Canvas& image, int x, int y, float alpha = 1);

    // A 64-bit hash of the pixels, to tell whether a frame changed without keeping a copy.
    std::uint64_t hash() const;

    bool write_png(const std::filesystem::path& path) const;

private:
    int width_, height_;
    std::vector<std::uint8_t> pixels_;

    void blend(int x, int y, Color color, float coverage);
    // Blends `color` at full coverage over pixels `from` to `to` (exclusive) of row `y`.
    void fill_span(int y, int from, int to, Color color);
    float measure(Font& font, const std::u32string& text, float size);
};

} // namespace knockthru
