#pragma once

#include "config.hpp"
#include "knock_detector.hpp"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace knockthru {

// Records 10 knocks of one kind at lowered thresholds and sets that kind's spike
// and approach thresholds. docs/calibration.md explains the rules and where each
// number comes from.
class Calibrator {
public:
    static constexpr int kWanted = 10;
    static constexpr double kSeconds = 90;
    // s. The hand coming down from pressing Start once counted as a knock 1 s in.
    static constexpr double kGetReady = 3;
    static constexpr double kPace = 1; // s between counted knocks, which the pacing ring shows
    static constexpr double kBurstGap = 0.25;   // s, knocks closer than this are one burst
    static constexpr double kQuietBefore = 0.6; // s without another knock before a burst
    static constexpr double kMinApproach = 0.25; // m/s
    static constexpr double kStillSpeed = 0.6;  // m/s, mean speed before a surface swing

    // TooSoon: before kGetReady, or within kPace of the last counted knock.
    // TooLight: the right kind, but its spike is under floor().
    enum class Heard { Counted, OtherKind, TooSoon, TooLight, Ignored };
    static constexpr int kHeardKinds = 5;
    struct Result {
        Heard heard;
        Knock knock; // the burst's knock with the fastest approach
    };

    Calibrator(KnockKind kind, const DetectorConfig& base, double start);

    const DetectorConfig& detector_config() const { return config_; }

    // Takes every knock the detector judged, in time order.
    void hear(const Knock& knock);
    // A burst finishes kBurstGap after its last knock, so call this every loop.
    std::optional<Result> next(double now);

    KnockKind kind() const { return kind_; }
    int count() const { return static_cast<int>(peaks_.size()); }
    bool done() const { return count() >= kWanted; }
    double seconds_left(double now) const;
    int heard(Heard heard) const { return heard_[static_cast<int>(heard)]; }
    double weakest() const;
    const std::vector<double>& peaks() const { return peaks_; }
    const std::vector<double>& speeds() const { return speeds_; } // approach speeds, m/s

    static double peak_threshold(KnockKind kind, const std::vector<double>& peaks); // m/s^2
    static double floor(KnockKind kind); // the lowest spike threshold calibration sets
    static double approach_threshold(const std::vector<double>& speeds); // m/s

    KindCalibration result() const; // call after done()

    // `base` with this kind's new thresholds, for the try step before saving.
    DetectorConfig trial_config(const DetectorConfig& base) const;

    KindCalibration save() const; // writes calibration.json

private:
    KnockKind kind_;
    DetectorConfig config_;
    double start_;
    std::vector<Knock> burst_;
    bool burst_quiet_ = false; // nothing came in the kQuietBefore before the burst
    double last_heard_ = -1e9; // the last knock of the previous burst
    double last_counted_ = -1e9;
    std::array<int, kHeardKinds> heard_{};
    std::vector<Result> results_; // oldest first

    void finish_burst();
    std::vector<double> peaks_, speeds_;
};

// One line for the log and the terminal.
std::string thresholds_text(const KindCalibration& calibration);

} // namespace knockthru
