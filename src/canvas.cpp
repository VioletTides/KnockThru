#include "canvas.hpp"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stb_truetype.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace knockthru {
namespace {

std::u32string decode_utf8(const std::string& text) {
    std::u32string out;
    for (std::size_t i = 0; i < text.size();) {
        const auto byte = static_cast<unsigned char>(text[i]);
        const int length = byte < 0x80 ? 1 : byte < 0xE0 ? 2 : byte < 0xF0 ? 3 : 4;
        char32_t code = length == 1 ? byte : byte & (0x7F >> length);
        for (int k = 1; k < length && i + k < text.size(); ++k) code = (code << 6) | (text[i + k] & 0x3F);
        out += code;
        i += length;
    }
    return out;
}

float clamp01(float value) { return std::min(1.0f, std::max(0.0f, value)); }

} // namespace

struct Font::Impl {
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
};

Font::Font(const std::filesystem::path& path) : impl_(std::make_unique<Impl>()) {
    std::ifstream stream(path, std::ios::binary);
    impl_->data.assign(std::istreambuf_iterator<char>(stream), {});
    if (impl_->data.empty() || !stbtt_InitFont(&impl_->info, impl_->data.data(), 0))
        throw std::runtime_error("Cannot load font " + path.string());
}

Font::~Font() = default;

const Font::Glyph& Font::glyph(char32_t codepoint, float size) {
    const auto key = std::make_pair(codepoint, static_cast<int>(size * 4));
    if (const auto found = cache_.find(key); found != cache_.end()) return found->second;
    Glyph glyph;
    const float scale = stbtt_ScaleForPixelHeight(&impl_->info, size);
    int advance = 0, bearing = 0;
    stbtt_GetCodepointHMetrics(&impl_->info, static_cast<int>(codepoint), &advance, &bearing);
    glyph.advance = advance * scale;
    unsigned char* bitmap = stbtt_GetCodepointBitmap(&impl_->info, 0, scale, static_cast<int>(codepoint),
        &glyph.width, &glyph.height, &glyph.x_offset, &glyph.y_offset);
    if (bitmap) {
        glyph.coverage.assign(bitmap, bitmap + glyph.width * glyph.height);
        stbtt_FreeBitmap(bitmap, nullptr);
    }
    return cache_.emplace(key, std::move(glyph)).first->second;
}

float Font::kerning(char32_t left, char32_t right, float size) const {
    return stbtt_GetCodepointKernAdvance(&impl_->info, static_cast<int>(left), static_cast<int>(right)) *
        stbtt_ScaleForPixelHeight(&impl_->info, size);
}

float Font::ascent(float size) const {
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&impl_->info, &ascent, &descent, &gap);
    return ascent * stbtt_ScaleForPixelHeight(&impl_->info, size);
}

Canvas::Canvas(int width, int height)
    : width_(width), height_(height), pixels_(static_cast<std::size_t>(width) * height * 4, 0) {}

void Canvas::clear(Color color) {
    for (std::size_t i = 0; i < pixels_.size(); i += 4) {
        pixels_[i] = color.r;
        pixels_[i + 1] = color.g;
        pixels_[i + 2] = color.b;
        pixels_[i + 3] = color.a;
    }
}

void Canvas::blend(int x, int y, Color color, float coverage) {
    if (x < 0 || y < 0 || x >= width_ || y >= height_ || coverage <= 0) return;
    auto* p = &pixels_[(static_cast<std::size_t>(y) * width_ + x) * 4];
    // A fully covered pixel of an opaque color, or one over transparency, is the color itself.
    if (coverage >= 1 && (color.a == 255 || p[3] == 0)) {
        p[0] = color.r;
        p[1] = color.g;
        p[2] = color.b;
        p[3] = color.a;
        return;
    }
    const float a = coverage * color.a / 255.0f;
    const float out_a = a + p[3] / 255.0f * (1 - a);
    if (out_a <= 0) return;
    const auto mix = [&](std::uint8_t src, std::uint8_t dst) {
        return static_cast<std::uint8_t>(std::lround((src * a + dst * (p[3] / 255.0f) * (1 - a)) / out_a));
    };
    p[0] = mix(color.r, p[0]);
    p[1] = mix(color.g, p[1]);
    p[2] = mix(color.b, p[2]);
    p[3] = static_cast<std::uint8_t>(std::lround(out_a * 255));
}

void Canvas::rounded_rect(Rect rect, float radius, Color color) {
    radius = std::min({radius, rect.w / 2, rect.h / 2});
    const int x0 = static_cast<int>(std::floor(rect.x)), x1 = static_cast<int>(std::ceil(rect.x + rect.w));
    const int y0 = static_cast<int>(std::floor(rect.y)), y1 = static_cast<int>(std::ceil(rect.y + rect.h));
    const auto coverage = [&](int x, int y) {
        const float px = x + 0.5f, py = y + 0.5f;
        // Signed distance to the rounded rectangle, negative inside.
        const float dx = std::max(std::abs(px - (rect.x + rect.w / 2)) - (rect.w / 2 - radius), 0.0f);
        const float dy = std::max(std::abs(py - (rect.y + rect.h / 2)) - (rect.h / 2 - radius), 0.0f);
        const float distance = std::sqrt(dx * dx + dy * dy) - radius;
        const float edge_x = std::min(px - rect.x, rect.x + rect.w - px) + 0.5f;
        const float edge_y = std::min(py - rect.y, rect.y + rect.h - py) + 0.5f;
        return clamp01(0.5f - distance) * clamp01(edge_x) * clamp01(edge_y);
    };
    // The shape is convex, so each row's fully covered pixels form one run. Only
    // the pixels before and after it need the distance math.
    for (int y = y0; y < y1; ++y) {
        int left = x0, right = x1 - 1;
        for (; left <= right; ++left) {
            const float c = coverage(left, y);
            if (c >= 1) break;
            blend(left, y, color, c);
        }
        for (; right > left; --right) {
            const float c = coverage(right, y);
            if (c >= 1) break;
            blend(right, y, color, c);
        }
        for (int x = left; x <= right; ++x) blend(x, y, color, 1);
    }
}

void Canvas::circle(float cx, float cy, float radius, Color color) {
    arc(cx, cy, 0, radius, 0, 7, color);
}

void Canvas::polygon(const std::vector<std::vector<Point>>& rings, Color color) {
    float top = static_cast<float>(height_), bottom = 0, left = static_cast<float>(width_), right = 0;
    for (const auto& ring : rings) {
        for (const auto& p : ring) {
            top = std::min(top, p.y);
            bottom = std::max(bottom, p.y);
            left = std::min(left, p.x);
            right = std::max(right, p.x);
        }
    }
    const int first_x = std::max(0, static_cast<int>(left)), last_x = std::min(width_, static_cast<int>(std::ceil(right)) + 1);
    // Four sample lines per pixel row. Each span adds its exact horizontal coverage.
    constexpr int kSamples = 4;
    std::vector<float> coverage(width_ + 1), crossings;
    for (int y = std::max(0, static_cast<int>(top)); y < std::min(height_, static_cast<int>(std::ceil(bottom))); ++y) {
        std::fill(coverage.begin() + first_x, coverage.begin() + last_x, 0.0f);
        for (int sample = 0; sample < kSamples; ++sample) {
            const float sy = y + (sample + 0.5f) / kSamples;
            crossings.clear();
            for (const auto& ring : rings) {
                for (std::size_t i = 0; i < ring.size(); ++i) {
                    const Point a = ring[i], b = ring[(i + 1) % ring.size()];
                    if ((a.y <= sy) != (b.y <= sy)) crossings.push_back(a.x + (sy - a.y) * (b.x - a.x) / (b.y - a.y));
                }
            }
            std::sort(crossings.begin(), crossings.end());
            for (std::size_t i = 0; i + 1 < crossings.size(); i += 2) {
                const float x0 = std::clamp(crossings[i], 0.0f, static_cast<float>(width_));
                const float x1 = std::clamp(crossings[i + 1], 0.0f, static_cast<float>(width_));
                const int i0 = static_cast<int>(x0), i1 = static_cast<int>(x1);
                if (i0 == i1) {
                    coverage[i0] += (x1 - x0) / kSamples;
                    continue;
                }
                coverage[i0] += (i0 + 1 - x0) / kSamples;
                for (int x = i0 + 1; x < i1; ++x) coverage[x] += 1.0f / kSamples;
                coverage[i1] += (x1 - i1) / kSamples;
            }
        }
        for (int x = first_x; x < last_x; ++x) blend(x, y, color, clamp01(coverage[x]));
    }
}

void Canvas::arc(float cx, float cy, float inner, float outer, float from, float to, Color color) {
    const int x0 = static_cast<int>(std::floor(cx - outer - 1)), x1 = static_cast<int>(std::ceil(cx + outer + 1));
    const int y0 = static_cast<int>(std::floor(cy - outer - 1)), y1 = static_cast<int>(std::ceil(cy + outer + 1));
    const bool full = to - from >= 6.283f;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const float dx = x + 0.5f - cx, dy = cy - (y + 0.5f);
            const float r = std::sqrt(dx * dx + dy * dy);
            float coverage = clamp01(outer - r + 0.5f) * (inner > 0 ? clamp01(r - inner + 0.5f) : 1.0f);
            if (!full) {
                float angle = std::atan2(dy, dx);
                while (angle < from) angle += 6.2831853f;
                const float outside = std::min(angle - from, to - angle);
                // Round the arc ends by softening a few pixels past each end.
                coverage *= clamp01(outside * r + 0.5f);
            }
            blend(x, y, color, coverage);
        }
    }
}

float Canvas::measure(Font& font, const std::string& utf8, float size) {
    const auto text = decode_utf8(utf8);
    float width = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        width += font.glyph(text[i], size).advance;
        if (i + 1 < text.size()) width += font.kerning(text[i], text[i + 1], size);
    }
    return width;
}

std::string Canvas::fit(Font& font, const std::string& utf8, float size, float width) {
    if (measure(font, utf8, size) <= width) return utf8;
    auto text = decode_utf8(utf8);
    while (!text.empty()) {
        text.pop_back();
        std::string out;
        for (const char32_t c : text) {
            if (c < 0x80) out += static_cast<char>(c);
            else if (c < 0x800) out += {static_cast<char>(0xC0 | (c >> 6)), static_cast<char>(0x80 | (c & 0x3F))};
            else out += {static_cast<char>(0xE0 | (c >> 12)), static_cast<char>(0x80 | ((c >> 6) & 0x3F)),
                         static_cast<char>(0x80 | (c & 0x3F))};
        }
        out += "...";
        if (measure(font, out, size) <= width) return out;
    }
    return "";
}

std::vector<std::string> Canvas::wrap(Font& font, const std::string& utf8, float size, float width, int max_lines) {
    std::vector<std::string> lines;
    std::string line;
    std::size_t start = 0;
    while (start <= utf8.size()) {
        auto end = utf8.find(' ', start);
        if (end == std::string::npos) end = utf8.size();
        const auto word = utf8.substr(start, end - start);
        const auto candidate = line.empty() ? word : line + " " + word;
        if (!line.empty() && measure(font, candidate, size) > width) {
            lines.push_back(line);
            line = word;
        } else {
            line = candidate;
        }
        start = end + 1;
    }
    if (!line.empty()) lines.push_back(line);
    if (static_cast<int>(lines.size()) > max_lines) {
        std::string rest = lines[max_lines - 1];
        for (std::size_t i = max_lines; i < lines.size(); ++i) rest += " " + lines[i];
        lines.resize(max_lines);
        lines.back() = fit(font, rest, size, width);
    }
    return lines;
}

void Canvas::text(Font& font, const std::string& utf8, float x, float y, float size, Color color, Align align) {
    const float width = measure(font, utf8, size);
    if (align == Align::Center) x -= width / 2;
    if (align == Align::Right) x -= width;
    // Centers the cap height on y. Cap height is about 0.7 of the pixel size.
    const float baseline = y + size * 0.36f;
    const auto text = decode_utf8(utf8);
    float pen = x;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto& glyph = font.glyph(text[i], size);
        const int left = static_cast<int>(std::lround(pen)) + glyph.x_offset;
        const int top = static_cast<int>(std::lround(baseline)) + glyph.y_offset;
        for (int gy = 0; gy < glyph.height; ++gy)
            for (int gx = 0; gx < glyph.width; ++gx)
                blend(left + gx, top + gy, color, glyph.coverage[gy * glyph.width + gx] / 255.0f);
        pen += glyph.advance;
        if (i + 1 < text.size()) pen += font.kerning(text[i], text[i + 1], size);
    }
}

void Canvas::draw(const Canvas& image, int x, int y, float alpha) {
    for (int iy = 0; iy < image.height_; ++iy)
        for (int ix = 0; ix < image.width_; ++ix) {
            const auto* p = &image.pixels_[(static_cast<std::size_t>(iy) * image.width_ + ix) * 4];
            blend(x + ix, y + iy, {p[0], p[1], p[2], 255}, alpha * p[3] / 255.0f);
        }
}

bool Canvas::write_png(const std::filesystem::path& path) const {
    return stbi_write_png(path.c_str(), width_, height_, 4, pixels_.data(), width_ * 4) != 0;
}

} // namespace knockthru
