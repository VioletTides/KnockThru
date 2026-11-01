#pragma once

#include <array>
#include <deque>
#include <optional>
#include <string>

namespace knockthru {

using Vec3 = std::array<double, 3>;

enum class Device { Hmd, Left, Right };

// One reading of a tracked device's pose.
struct PoseSample {
    Device device;
    double t;
    Vec3 position, velocity;
};

double norm(const Vec3& v);
// m/s^2, the change from velocity `from` to velocity `to` over `seconds`.
double acceleration(const Vec3& from, const Vec3& to, double seconds);

enum class KnockKind { Together, Surface, Headset };
const char* to_string(KnockKind kind);
std::optional<KnockKind> parse_knock_kind(const std::string& name);

// Thresholds come from the Steam Frame recordings in tests/data/.
// docs/knock-detection.md explains each one.
struct DetectorConfig {
    double controller_peak = 60;   // m/s^2, spike on the knocking controller
    double confirmed_peak =
        40;    // m/s^2, controller spike for a together knock, which the other controller confirms
    double headset_knock_peak = 40; // m/s^2, controller spike for a headset knock, which the headset confirms
    double partner_peak = 20; // m/s^2, spike on the other controller for a together knock
    double headset_peak = 25; // m/s^2, spike on the headset for a headset knock
    double max_speed_after = 0.4; // m/s, 30 ms after the spike
    double surface_speed_after = 0.55; // m/s, table knocks rebound
    double max_speed_before = 1.8; // m/s, 30 ms before the spike
    double surface_speed_before = 2.2; // m/s, knocks on a leg come in faster
    double together_distance = 0.15; // m, controller separation for a together knock
    double same_spot = 0.06; // m, surface knocks in one sequence must land this close. Leg knocks spread more
                             // than desk knocks.
    double merge_s = 0.08; // spikes closer than this are one knock (bounces)
    double min_gap_s = 0.15; // closer surface knocks are table bounces
    double fast_gap_s = 0.105; // closer together or headset knocks are rebounds
    double max_gap_s = 0.35; // a sequence ends after this long without a knock
    double quiet_before_s = 0.8; // a sequence must start this long after the previous one ended
    // m/s, the slowest approach that counts, per KnockKind. Calibration sets these.
    std::array<double, 3> min_approach{0, 0, 0};
    std::array<bool, 3> enabled{true, true, true}; // indexed by KnockKind
};

struct Knock {
    double t;
    KnockKind kind;
    int source; // Device as int, or -1 for both controllers
    Vec3 position;
    double peak;
    double speed_before = 0; // m/s, mean speed of the knocking controller 0.6 to 0.15 s before the spike
    // m/s, the knocking controller's fastest speed in the 100 ms before the spike. It
    // shows how hard a knock was more steadily than `peak`, which divides by pose
    // update gaps that vary from 2 to 7 ms.
    double approach_speed = 0;
};

struct KnockEvent {
    double t; // time of the last knock in the sequence
    KnockKind kind;
    int count; // 2 or 3
};

// How the detector judged one spike, for the debug view.
enum class Verdict {
    Counted, // joined the current sequence
    BelowPeak, // under the peak its kind needs
    FastBefore, // the controller came in faster than a knock does
    MovingAfter, // the controller did not nearly stop
    Untracked, // no samples 30 ms before or after
    Bounce, // within merge_s of the previous knock
    Rebound, // within min_gap_s or fast_gap_s of the previous knock in the sequence
};
const char* to_string(Verdict verdict);

// A spike of at least half the lowest peak threshold, which is close enough to
// a knock to be worth showing. Weaker spikes happen all the time in play.
struct Candidate {
    Knock knock;
    Verdict verdict;
    double threshold; // m/s^2, the peak this kind needs
    double speed_30ms_before = 0, speed_30ms_after = 0; // m/s, of the knocking controller
    double before_limit = 0, after_limit = 0; // m/s, the speeds this kind allows
};

// Why a finished sequence did or did not act, in the order the detector checks.
enum class SequenceVerdict {
    Action,
    WrongCount, // not 2 or 3 knocks
    NotQuiet, // started within quiet_before_s of the previous sequence
    MixedKinds,
    MixedSources, // different devices, or a together knock among single ones
    MixedSpots, // surface knocks further apart than same_spot
    TooSlow, // no knock approached at min_approach
    KindOff,
};
const char* to_string(SequenceVerdict verdict);

struct Sequence {
    std::deque<Knock> knocks;
    SequenceVerdict verdict;
    double fastest_approach; // m/s
    double needed_approach; // m/s, min_approach for the kind of the last knock
};

// Streaming detector. Feed every pose sample in time order with push(), then
// call poll() with the current time. A knock is judged about 50 ms after it
// happens, and a sequence is reported max_gap_s after its last knock.
class KnockDetector {
public:
    explicit KnockDetector(DetectorConfig config = {});

    void push(Device device, double t, const Vec3& position, const Vec3& velocity);
    std::optional<KnockEvent> poll(double now);

    // Every knock judged so far, for logging and calibration. Cleared on read.
    std::deque<Knock> take_knocks();
    // Every candidate spike and finished sequence so far, for the debug view. Cleared on read.
    std::deque<Candidate> take_candidates();
    std::deque<Sequence> take_sequences();

    const DetectorConfig& config() const { return config_; }
    // Changes the thresholds and keeps the pose history, so knocks right after the change still count.
    void set_config(const DetectorConfig& config) { config_ = config; }

    // The sequence still open, oldest knock first.
    const std::deque<Knock>& sequence() const { return sequence_; }

private:
    struct Sample {
        double t;
        Vec3 position;
        Vec3 velocity;
        double accel; // m/s^2 since the previous changed sample
    };
    using History = std::deque<Sample>;

    static constexpr double kJudgeDelay = 0.05;
    static constexpr double kKeep = 0.8; // covers speed_before

    DetectorConfig config_;
    std::array<History, 3> history_;
    std::array<double, 3> judged_until_{{-1, -1, -1}};
    std::deque<Knock> sequence_;
    // The first reason the open sequence cannot act, or Action while it still can.
    SequenceVerdict mixed_ = SequenceVerdict::Action;
    double burst_end_t_ = -1e9;
    std::deque<Knock> log_;
    std::deque<Candidate> candidates_;
    std::deque<Sequence> sequences_;
    double last_knock_t_ = -1e9;

    std::optional<Candidate> judge(Device device, std::size_t index) const;
    Verdict add(const Knock& knock);
    void mix(SequenceVerdict reason);
    std::optional<KnockEvent> finish_sequence();
    static History::const_iterator first_from(const History& history, double t);
    static const Sample* nearest(const History& history, double t);
    static double peak_in(const History& history, double from, double to);
    static double mean_speed(const History& history, double from, double to);
    static double max_speed(const History& history, double from, double to);
};

} // namespace knockthru
