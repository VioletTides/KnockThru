#include "calibrator.hpp"

#include "config.hpp"

#include <algorithm>
#include <stdexcept>

namespace knockthru {

Calibrator::Calibrator(KnockKind kind, const DetectorConfig& base, double start)
    : kind_(kind), config_(base), start_(start) {
    config_.controller_peak = std::min(config_.controller_peak, 30.0);
    config_.confirmed_peak = std::min(config_.confirmed_peak, 12.0);
    config_.headset_knock_peak = std::min(config_.headset_knock_peak, 12.0);
    config_.partner_peak = std::min(config_.partner_peak, 8.0);
    config_.enabled = {true, true, true};
}

void Calibrator::hear(const Knock& knock) {
    if (!burst_.empty() && knock.t - burst_.back().t > kBurstGap) finish_burst();
    if (burst_.empty()) burst_quiet_ = knock.t - last_heard_ >= kQuietBefore;
    burst_.push_back(knock);
}

std::optional<Calibrator::Result> Calibrator::next(double now) {
    if (!burst_.empty() && now - burst_.back().t > kBurstGap) finish_burst();
    if (results_.empty()) return std::nullopt;
    const auto result = results_.front();
    results_.erase(results_.begin());
    return result;
}

void Calibrator::finish_burst() {
    const auto best = *std::max_element(burst_.begin(), burst_.end(),
        [](const Knock& a, const Knock& b) { return a.approach_speed < b.approach_speed; });
    const bool still = best.kind != KnockKind::Surface || burst_.front().speed_before < kStillSpeed;
    const bool deliberate = burst_quiet_ && still && best.approach_speed >= kMinApproach;
    // The same clock as the pacing ring: from the start, then from the last counted knock.
    const double first = burst_.front().t;
    const bool early = first < start_ + kGetReady || first - last_counted_ < kPace;
    last_heard_ = burst_.back().t;
    burst_.clear();
    Heard heard = Heard::Ignored;
    if (deliberate && !done())
        heard = early ? Heard::TooSoon : best.kind != kind_ ? Heard::OtherKind
            : best.peak < floor(kind_) ? Heard::TooLight : Heard::Counted;
    if (heard == Heard::Counted) {
        last_counted_ = best.t;
        peaks_.push_back(best.peak);
        speeds_.push_back(best.approach_speed);
    }
    ++heard_[static_cast<int>(heard)];
    results_.push_back({heard, best});
}

double Calibrator::seconds_left(double now) const { return std::max(0.0, kSeconds - (now - start_)); }

double Calibrator::weakest() const {
    if (peaks_.empty()) throw std::logic_error("No knocks recorded");
    return *std::min_element(peaks_.begin(), peaks_.end());
}

double Calibrator::floor(KnockKind kind) { return kind == KnockKind::Surface ? 50.0 : 20.0; }

double Calibrator::peak_threshold(KnockKind kind, const std::vector<double>& peaks) {
    if (peaks.empty()) throw std::logic_error("No knocks recorded");
    return std::clamp(0.6 * *std::min_element(peaks.begin(), peaks.end()), floor(kind), 150.0);
}

double Calibrator::approach_threshold(const std::vector<double>& speeds) {
    if (speeds.empty()) throw std::logic_error("No knocks recorded");
    auto sorted = speeds;
    std::sort(sorted.begin(), sorted.end());
    // The second slowest, so one odd knock does not set the threshold alone.
    return 0.55 * sorted[std::min<std::size_t>(1, sorted.size() - 1)];
}

KindCalibration Calibrator::result() const {
    KindCalibration out;
    out.peak = peak_threshold(kind_, peaks_);
    out.approach = approach_threshold(speeds_);
    out.peaks = peaks_;
    out.speeds = speeds_;
    return out;
}

DetectorConfig Calibrator::trial_config(const DetectorConfig& base) const {
    DetectorConfig config = base;
    Calibration only;
    only[kind_] = result();
    apply_calibration(only, config);
    config.enabled = {true, true, true};
    return config;
}

KindCalibration Calibrator::save() const {
    auto calibration = load_calibration();
    calibration[kind_] = result();
    save_calibration(calibration);
    return *calibration[kind_];
}

} // namespace knockthru
