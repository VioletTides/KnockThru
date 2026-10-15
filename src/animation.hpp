#pragma once

// Easing curves and the knock timeline for the panel animations. The README
// animations in scripts/readme_media.py use the same curves and timing.

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <optional>

namespace knockthru::anim {

constexpr float kPi = 3.14159265f;

// Each curve maps 0..1 to 0..1.
inline float clamp(float t) { return std::min(1.0f, std::max(0.0f, t)); }
inline float ease_in(float t) { return std::pow(clamp(t), 3.0f); }
inline float ease_out(float t) { return 1 - std::pow(1 - clamp(t), 3.0f); }
inline float ease_in_out(float t) { return 0.5f - 0.5f * std::cos(kPi * clamp(t)); }
// Overshoots by about 10 percent, then settles.
inline float ease_out_back(float t) {
    constexpr float overshoot = 1.7f;
    t = clamp(t) - 1;
    return 1 + t * t * ((overshoot + 1) * t + overshoot);
}

using Curve = float (*)(float);

struct Key {
    float t, value;
    Curve curve; // shapes the segment that ends at this key
};

// Interpolates between keys in time order.
inline float keyframes(float t, std::initializer_list<Key> keys) {
    const Key* previous = nullptr;
    for (const auto& key : keys) {
        if (t <= key.t) {
            if (!previous) return key.value;
            return previous->value + (key.value - previous->value) * key.curve((t - previous->t) / (key.t - previous->t));
        }
        previous = &key;
    }
    return previous ? previous->value : 0;
}

// One knock timeline for every animation: the first contact at 0.5 s, then one
// every 0.3 s.
constexpr float kFirstKnock = 0.5f, kKnockGap = 0.3f;

inline float knock_time(int i, float start = kFirstKnock) { return start + i * kKnockGap; }

// How far a controller is from the contact point, from 0 (touching) to 1 (rest).
// Each knock pulls back a little, speeds up into the contact, and rebounds. After
// the last knock it settles back to rest.
inline float knock_motion(float t, int knocks, float start = kFirstKnock) {
    const float first = knock_time(0, start), last = knock_time(knocks - 1, start);
    if (t <= first)
        return keyframes(t, {{first - 0.3f, 1.0f, ease_in_out}, {first - 0.13f, 1.18f, ease_in_out}, {first, 0.0f, ease_in}});
    for (int i = 1; i < knocks; ++i) {
        const float contact = knock_time(i, start), previous = knock_time(i - 1, start);
        if (t <= contact) return keyframes(t, {{previous, 0.0f, ease_in}, {contact - kKnockGap / 2, 0.55f, ease_out}, {contact, 0.0f, ease_in}});
    }
    return keyframes(t, {{last, 0.0f, ease_in}, {last + 0.35f, 1.0f, ease_out_back}});
}

// How far the latest impact has played, from 0 (just made contact) to 1 (faded), or nothing.
inline std::optional<float> impact(float t, int knocks, float start = kFirstKnock) {
    for (int i = knocks - 1; i >= 0; --i) {
        const float contact = knock_time(i, start);
        if (contact <= t && t < contact + 0.3f) return (t - contact) / 0.3f;
    }
    return std::nullopt;
}

// A small side-to-side shake, one burst per buzz, in pixels.
inline float buzz(float t, float action, int count) {
    for (int i = 0; i < count; ++i) {
        const float local = t - action - i * 0.18f;
        if (local >= 0 && local < 0.12f) return 3 * std::sin(local * 2 * kPi * 25) * (1 - local / 0.12f);
    }
    return 0;
}

} // namespace knockthru::anim
