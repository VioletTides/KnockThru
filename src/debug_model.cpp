#include "debug_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace knockthru {

std::string number(double value, int decimals) {
    char text[32];
    std::snprintf(text, sizeof text, "%.*f", decimals, value);
    return text;
}

namespace {

double norm(const Vec3& v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

std::string title_of(int source) {
    if (source < 0) return "Both controllers";
    if (source == static_cast<int>(Device::Left)) return "Left controller";
    if (source == static_cast<int>(Device::Right)) return "Right controller";
    return "Headset";
}

const char* verdict_word(Verdict verdict) {
    switch (verdict) {
    case Verdict::BelowPeak: return "Too light";
    case Verdict::FastBefore: return "Came in too fast";
    case Verdict::MovingAfter: return "Kept moving";
    case Verdict::Untracked: return "Not tracked";
    case Verdict::Counted:
    case Verdict::Bounce:
    case Verdict::Rebound: break;
    }
    return "";
}

const char* action_word(KnockAction action) {
    switch (action) {
    case KnockAction::Passthrough: return "Passthrough";
    case KnockAction::Style: return "Style";
    case KnockAction::PerfGraph: return "Perf graph";
    case KnockAction::None: break;
    }
    return "No action";
}

const char* verdict_word(SequenceVerdict verdict) {
    switch (verdict) {
    case SequenceVerdict::Action: return "No action";
    case SequenceVerdict::WrongCount: return "Wrong count";
    case SequenceVerdict::NotQuiet: return "Too soon";
    case SequenceVerdict::MixedKinds: return "Mixed kinds";
    case SequenceVerdict::MixedSources: return "Mixed hands";
    case SequenceVerdict::MixedSpots: return "Moved between knocks";
    case SequenceVerdict::TooSlow: return "Too slow";
    case SequenceVerdict::KindOff: return "Kind is off";
    }
    return "";
}

const char* kind_word(KnockKind kind) {
    switch (kind) {
    case KnockKind::Together: return "Together";
    case KnockKind::Surface: return "Surface";
    case KnockKind::Headset: return "Headset";
    }
    return "";
}

std::string set_title(const Sequence& sequence) {
    const auto count = static_cast<int>(sequence.knocks.size());
    std::string title = std::string(kind_word(sequence.knocks.back().kind)) + " knock";
    if (count == 2 || count == 3) return title + " " + count_word(count);
    if (count >= 4) return title + ", " + std::to_string(count) + " times";
    return title;
}

std::string spike_detail(const Candidate& c) {
    switch (c.verdict) {
    case Verdict::BelowPeak: return number(c.knock.peak, 0) + " m/s² (min " + number(c.threshold, 0) + ")";
    case Verdict::FastBefore:
        return number(c.speed_30ms_before, 2) + " m/s before (max " + number(c.before_limit, 2) + ")";
    case Verdict::MovingAfter:
        return number(c.speed_30ms_after, 2) + " m/s after (max " + number(c.after_limit, 2) + ")";
    case Verdict::Untracked:
    case Verdict::Counted:
    case Verdict::Bounce:
    case Verdict::Rebound: break;
    }
    return "";
}

} // namespace

void DebugModel::push(const PoseSample& sample) {
    const auto d = static_cast<std::size_t>(sample.device);
    // A time or velocity that is not a number would put a sample in no bucket.
    if (!std::isfinite(sample.t) || !std::isfinite(norm(sample.velocity))) return;
    // SteamVR repeats the last pose between controller updates, as the detector also assumes.
    if (last_t_[d] >= 0 && (sample.velocity == last_velocity_[d] || sample.t <= last_t_[d])) return;
    float accel = 0;
    if (last_t_[d] >= 0) {
        const Vec3 change{sample.velocity[0] - last_velocity_[d][0],
            sample.velocity[1] - last_velocity_[d][1], sample.velocity[2] - last_velocity_[d][2]};
        accel = static_cast<float>(norm(change) / (sample.t - last_t_[d]));
    }
    last_t_[d] = sample.t;
    last_velocity_[d] = sample.velocity;
    ring_.push_back({sample.device, sample.t, accel});
    // Devices push in time order, but one may lag another by a tick.
    while (!ring_.empty() && ring_.front().t < last_t_[d] - kKeepSeconds) ring_.pop_front();
}

void DebugModel::add_feed(FeedEntry entry) {
    feed_.push_back(std::move(entry));
    if (feed_.size() > kFeedSize) feed_.pop_front();
}

void DebugModel::add_candidate(const Candidate& c) {
    const bool near_miss = c.verdict == Verdict::FastBefore || c.verdict == Verdict::MovingAfter
        || c.verdict == Verdict::Untracked
        || (c.verdict == Verdict::BelowPeak && c.knock.peak >= 0.75 * c.threshold);
    if (!near_miss) return;
    add_feed({c.knock.t, title_of(c.knock.source), verdict_word(c.verdict), spike_detail(c), false});
}

void DebugModel::add_set(const Sequence& s, SetOutcome outcome, KnockAction action) {
    if (s.knocks.empty()) return;
    FeedEntry entry{s.knocks.back().t, set_title(s), verdict_word(s.verdict), "", false};
    if (s.verdict == SequenceVerdict::Action) {
        switch (outcome) {
        case SetOutcome::Ran:
            entry.verdict = action_word(action);
            entry.good = true;
            break;
        case SetOutcome::Paused: entry.verdict = "Paused"; break;
        case SetOutcome::Blocked: entry.verdict = "Off in this game"; break;
        case SetOutcome::NoAction: break;
        }
    } else if (s.verdict == SequenceVerdict::TooSlow) {
        entry.detail =
            number(s.fastest_approach, 2) + " m/s approach (min " + number(s.needed_approach, 2) + ")";
    }
    add_feed(std::move(entry));
}

std::vector<float> DebugModel::accel(Device device, double now, double seconds) const {
    const auto count = static_cast<long>(std::lround(seconds / kBucketSeconds));
    std::vector<float> out(static_cast<std::size_t>(std::max(count, 0L)), 0.0f);
    // Buckets sit on a fixed time grid, so a spike keeps its bucket as the graph scrolls. The bucket that
    // `now` falls in is still filling, and drawn it would drop to the floor at the graph's right edge.
    const long last = static_cast<long>(std::floor(now / kBucketSeconds)) - 1;
    const long first = last - count + 1;
    for (auto it = ring_.rbegin(); it != ring_.rend() && it->t >= first * kBucketSeconds; ++it) {
        if (it->device != device) continue;
        const long bucket = static_cast<long>(std::floor(it->t / kBucketSeconds));
        if (bucket > last) continue;
        auto& slot = out[static_cast<std::size_t>(bucket - first)];
        slot = std::max(slot, it->accel);
    }
    return out;
}

double DebugModel::pose_rate(Device device, double now) const {
    int count = 0;
    for (auto it = ring_.rbegin(); it != ring_.rend() && it->t > now - 1; ++it)
        if (it->device == device && it->t <= now) ++count;
    return count;
}

} // namespace knockthru
