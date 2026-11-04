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

const char* to_string(Verdict verdict) {
    switch (verdict) {
    case Verdict::Counted: return "counted";
    case Verdict::BelowPeak: return "below peak";
    case Verdict::FastBefore: return "too fast before";
    case Verdict::MovingAfter: return "still moving after";
    case Verdict::Untracked: return "untracked";
    case Verdict::Bounce: return "bounce";
    case Verdict::Rebound: return "rebound";
    }
    return "unknown";
}

const char* to_string(SequenceVerdict verdict) {
    switch (verdict) {
    case SequenceVerdict::Action: return "action";
    case SequenceVerdict::WrongCount: return "wrong count";
    case SequenceVerdict::NotQuiet: return "not quiet before";
    case SequenceVerdict::MixedKinds: return "mixed kinds";
    case SequenceVerdict::MixedSources: return "mixed sources";
    case SequenceVerdict::MixedSpots: return "mixed spots";
    case SequenceVerdict::TooSlow: return "approach too slow";
    case SequenceVerdict::KindOff: return "kind off";
    }
    return "unknown";
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
// the threshold, after which the controller nearly stops. Returns nothing for
// a sample that is not a local peak or is under half the lowest threshold.
std::optional<Candidate> KnockDetector::judge(Device device, std::size_t i) const {
    const auto& history = history_[index(device)];
    const auto& sample = history[i];
    const double lowest =
        std::min({config_.controller_peak, config_.confirmed_peak, config_.headset_knock_peak});
    if (sample.accel < lowest / 2) return std::nullopt;
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
    Candidate candidate{knock, Verdict::Counted, threshold};
    candidate.before_limit = surface ? config_.surface_speed_before : config_.max_speed_before;
    candidate.after_limit = surface ? config_.surface_speed_after : config_.max_speed_after;
    const auto* before = nearest(history, sample.t - 0.03);
    const auto* after = nearest(history, sample.t + 0.03);
    if (before) candidate.speed_30ms_before = norm(before->velocity);
    if (after) candidate.speed_30ms_after = norm(after->velocity);
    if (sample.accel < threshold)
        candidate.verdict = Verdict::BelowPeak;
    else if (!before || !after)
        candidate.verdict = Verdict::Untracked;
    else if (candidate.speed_30ms_before >= candidate.before_limit)
        candidate.verdict = Verdict::FastBefore;
    else if (candidate.speed_30ms_after >= candidate.after_limit)
        candidate.verdict = Verdict::MovingAfter;
    return candidate;
}

void KnockDetector::mix(SequenceVerdict reason) {
    if (mixed_ == SequenceVerdict::Action) mixed_ = reason;
}

Verdict KnockDetector::add(const Knock& knock) {
    if (knock.t - last_knock_t_ < config_.merge_s) return Verdict::Bounce; // also the other hand of one knock
    last_knock_t_ = knock.t;
    log_.push_back(knock);

    // Every knock within max_gap_s of the previous one joins the same burst.
    // A burst that mixes kinds, sources, or surface spots is not an action,
    // which stops the tail of a clapping run from reading as a double.
    if (sequence_.empty()) {
        // A real "knock knock" starts from stillness, not right after other knocks.
        if (knock.t - burst_end_t_ < config_.quiet_before_s) mix(SequenceVerdict::NotQuiet);
    } else {
        const auto& last = sequence_.back();
        // Tables bounce later than controllers or the headset rebound, so fast
        // together and headset doubles can sit closer than surface ones.
        const double gap = knock.kind == KnockKind::Surface ? config_.min_gap_s : config_.fast_gap_s;
        if (knock.t - last.t < gap) return Verdict::Rebound;
        if (knock.kind != last.kind)
            mix(SequenceVerdict::MixedKinds);
        else if (knock.source != last.source)
            mix(SequenceVerdict::MixedSources);
        else if (knock.kind == KnockKind::Surface
            && distance(knock.position, last.position) > config_.same_spot)
            mix(SequenceVerdict::MixedSpots);
    }
    sequence_.push_back(knock);
    return Verdict::Counted;
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
    const double needed = config_.min_approach[index(last.kind)];
    burst_end_t_ = last.t;
    auto verdict = SequenceVerdict::Action;
    if (count != 2 && count != 3)
        verdict = SequenceVerdict::WrongCount;
    else if (mixed_ != SequenceVerdict::Action)
        verdict = mixed_;
    else if (fastest < needed)
        verdict = SequenceVerdict::TooSlow;
    else if (!config_.enabled[index(last.kind)])
        verdict = SequenceVerdict::KindOff;
    sequences_.push_back({std::move(sequence_), verdict, fastest, needed});
    sequence_.clear();
    mixed_ = SequenceVerdict::Action;
    if (verdict != SequenceVerdict::Action) return std::nullopt;
    return KnockEvent{last.t, last.kind, count};
}

std::optional<KnockEvent> KnockDetector::poll(double now) {
    const double horizon = now - kJudgeDelay;
    std::deque<Candidate> found;
    for (const auto device : {Device::Left, Device::Right}) {
        const auto& history = history_[index(device)];
        auto& judged = judged_until_[index(device)];
        // Samples up to `judged` were judged by an earlier call.
        auto i = static_cast<std::size_t>(std::upper_bound(history.begin(), history.end(), judged,
                                              [](double t, const Sample& s) { return t < s.t; })
            - history.begin());
        for (; i < history.size() && history[i].t <= horizon; ++i) {
            if (auto candidate = judge(device, i)) found.push_back(*candidate);
            judged = history[i].t;
        }
    }
    std::sort(found.begin(), found.end(),
        [](const Candidate& a, const Candidate& b) { return a.knock.t < b.knock.t; });
    for (auto& candidate : found) {
        if (candidate.verdict == Verdict::Counted) candidate.verdict = add(candidate.knock);
        candidates_.push_back(candidate);
    }

    if (!sequence_.empty() && horizon - sequence_.back().t > config_.max_gap_s) return finish_sequence();
    return std::nullopt;
}

std::deque<Knock> KnockDetector::take_knocks() {
    std::deque<Knock> knocks;
    knocks.swap(log_);
    return knocks;
}

std::deque<Candidate> KnockDetector::take_candidates() {
    std::deque<Candidate> candidates;
    candidates.swap(candidates_);
    return candidates;
}

std::deque<Sequence> KnockDetector::take_sequences() {
    std::deque<Sequence> sequences;
    sequences.swap(sequences_);
    return sequences;
}

} // namespace knockthru
