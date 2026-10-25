#include "knock_detector.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace knockthru {
namespace {

double norm(const Vec3& v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
double distance(const Vec3& a, const Vec3& b) { return norm({a[0] - b[0], a[1] - b[1], a[2] - b[2]}); }
int index(Device device) { return static_cast<int>(device); }
int index(KnockKind kind) { return static_cast<int>(kind); }

} // namespace

const char* to_string(KnockKind kind) {
    switch (kind) {
    case KnockKind::Together: return "together";
    case KnockKind::Surface: return "surface";
    case KnockKind::Headset: return "headset";
    }
    return "unknown";
}

std::optional<KnockKind> parse_knock_kind(const std::string& name) {
    if (name == "together") return KnockKind::Together;
    if (name == "surface") return KnockKind::Surface;
    if (name == "headset") return KnockKind::Headset;
    return std::nullopt;
}

KnockDetector::KnockDetector(DetectorConfig config) : config_(config) {}

void KnockDetector::push(Device device, double t, const Vec3& position, const Vec3& velocity) {
    auto& history = history_[index(device)];
    // SteamVR repeats the last pose between controller updates. Only a changed
    // velocity is a new measurement.
    if (!history.empty() && (history.back().velocity == velocity || t <= history.back().t)) return;
    double accel = 0;
    if (!history.empty()) {
        const auto& last = history.back();
        accel = norm({velocity[0] - last.velocity[0], velocity[1] - last.velocity[1],
                    velocity[2] - last.velocity[2]})
            / (t - last.t);
    }
    history.push_back({t, position, velocity, accel});
    while (!history.empty() && history.front().t < t - kKeep) history.pop_front();
}

// History is sorted by time, so each lookup starts with a binary search.
KnockDetector::History::const_iterator KnockDetector::first_from(const History& history, double t) {
    return std::lower_bound(
        history.begin(), history.end(), t, [](const Sample& s, double t) { return s.t < t; });
}

const KnockDetector::Sample* KnockDetector::nearest(const History& history, double t) {
    if (history.empty()) return nullptr;
    const auto after = first_from(history, t);
    if (after == history.begin()) return &*after;
    const auto before = std::prev(after);
    // The earlier sample wins a tie.
    if (after == history.end() || std::abs(before->t - t) <= std::abs(after->t - t)) return &*before;
    return &*after;
}

double KnockDetector::mean_speed(const History& history, double from, double to) {
    double sum = 0;
    int count = 0;
    for (auto it = first_from(history, from); it != history.end() && it->t <= to; ++it) {
        sum += norm(it->velocity);
        ++count;
    }
    return count ? sum / count : 0;
}

double KnockDetector::max_speed(const History& history, double from, double to) {
    double fastest = 0;
    for (auto it = first_from(history, from); it != history.end() && it->t <= to; ++it)
        fastest = std::max(fastest, norm(it->velocity));
    return fastest;
}

double KnockDetector::peak_in(const History& history, double from, double to) {
    double peak = 0;
    for (auto it = first_from(history, from); it != history.end() && it->t <= to; ++it)
        peak = std::max(peak, it->accel);
    return peak;
}

// Decides whether the controller sample at `i` is a knock: a local peak above
// the threshold, after which the controller nearly stops.
std::optional<Knock> KnockDetector::judge(Device device, std::size_t i) const {
    const auto& history = history_[index(device)];
    const auto& sample = history[i];
    if (sample.accel
        < std::min({config_.controller_peak, config_.confirmed_peak, config_.headset_knock_peak}))
        return std::nullopt;
    for (std::size_t k = i > 3 ? i - 3 : 0; k < std::min(history.size(), i + 4); ++k)
        if (history[k].accel > sample.accel) return std::nullopt;

    Knock knock{sample.t, KnockKind::Surface, index(device), sample.position, sample.accel,
        mean_speed(history, sample.t - 0.6, sample.t - 0.15),
        max_speed(history, sample.t - 0.1, sample.t - 0.005)};
    const auto& hmd = history_[index(Device::Hmd)];
    const auto& other = history_[index(device == Device::Left ? Device::Right : Device::Left)];
    const auto* other_now = nearest(other, sample.t);
    if (peak_in(hmd, sample.t - 0.03, sample.t + 0.03) > config_.headset_peak) {
        knock.kind = KnockKind::Headset;
        knock.source = index(Device::Hmd);
    } else if (other_now && peak_in(other, sample.t - 0.04, sample.t + 0.04) > config_.partner_peak
        && distance(sample.position, other_now->position) < config_.together_distance) {
        knock.kind = KnockKind::Together;
        knock.source = -1;
    }

    // A headset or together knock has a second witness, so a weaker spike is
    // enough. A table knock rebounds, so the controller may still be moving.
    const bool surface = knock.kind == KnockKind::Surface;
    const double threshold = surface        ? config_.controller_peak
        : knock.kind == KnockKind::Together ? config_.confirmed_peak
                                            : config_.headset_knock_peak;
    if (sample.accel < threshold) return std::nullopt;
    const auto* before = nearest(history, sample.t - 0.03);
    const auto* after = nearest(history, sample.t + 0.03);
    if (!before || !after
        || norm(before->velocity) >= (surface ? config_.surface_speed_before : config_.max_speed_before)
        || norm(after->velocity) >= (surface ? config_.surface_speed_after : config_.max_speed_after))
        return std::nullopt;
    return knock;
}

void KnockDetector::add(const Knock& knock) {
    if (knock.t - last_knock_t_ < config_.merge_s) return; // bounce or the other hand of one knock
    last_knock_t_ = knock.t;
    log_.push_back(knock);

    // Every knock within max_gap_s of the previous one joins the same burst.
    // A burst that mixes kinds, sources, or surface spots is not an action,
    // which stops the tail of a clapping run from reading as a double.
    if (sequence_.empty()) {
        // A real "knock knock" starts from stillness, not right after other knocks.
        if (knock.t - burst_end_t_ < config_.quiet_before_s) mixed_ = true;
    } else {
        const auto& last = sequence_.back();
        // Tables bounce later than controllers or the headset rebound, so fast
        // together and headset doubles can sit closer than surface ones.
        const double gap = knock.kind == KnockKind::Surface ? config_.min_gap_s : config_.fast_gap_s;
        if (knock.t - last.t < gap) return;
        if (knock.kind != last.kind || knock.source != last.source
            || (knock.kind == KnockKind::Surface
                && distance(knock.position, last.position) > config_.same_spot))
            mixed_ = true;
    }
    sequence_.push_back(knock);
}

std::optional<KnockEvent> KnockDetector::finish_sequence() {
    const auto count = static_cast<int>(sequence_.size());
    const auto last = sequence_.back();
    // A sequence needs one knock that approached at least min_approach. The
    // later knocks of a quick double start from the rebound, so they come in
    // slow. Slow knocks still join their sequence, so they keep clapping runs
    // and busy bursts from reading as actions. Dropping them earlier let two
    // leftover spikes in the play and noise recordings form false doubles.
    double fastest = 0;
    for (const auto& knock : sequence_) fastest = std::max(fastest, knock.approach_speed);
    const bool slow = fastest < config_.min_approach[index(last.kind)];
    burst_end_t_ = last.t;
    const bool mixed = mixed_;
    sequence_.clear();
    mixed_ = false;
    if (mixed || slow || (count != 2 && count != 3) || !config_.enabled[index(last.kind)])
        return std::nullopt;
    return KnockEvent{last.t, last.kind, count};
}

std::optional<KnockEvent> KnockDetector::poll(double now) {
    const double horizon = now - kJudgeDelay;
    std::deque<Knock> found;
    for (const auto device : {Device::Left, Device::Right}) {
        const auto& history = history_[index(device)];
        auto& judged = judged_until_[index(device)];
        // Samples up to `judged` were judged by an earlier call.
        auto i = static_cast<std::size_t>(std::upper_bound(history.begin(), history.end(), judged,
                                              [](double t, const Sample& s) { return t < s.t; })
            - history.begin());
        for (; i < history.size() && history[i].t <= horizon; ++i) {
            if (auto knock = judge(device, i)) found.push_back(*knock);
            judged = history[i].t;
        }
    }
    std::sort(found.begin(), found.end(), [](const Knock& a, const Knock& b) { return a.t < b.t; });
    for (const auto& knock : found) add(knock);

    if (!sequence_.empty() && horizon - sequence_.back().t > config_.max_gap_s) return finish_sequence();
    return std::nullopt;
}

std::deque<Knock> KnockDetector::take_knocks() {
    std::deque<Knock> knocks;
    knocks.swap(log_);
    return knocks;
}

} // namespace knockthru
