#include "calibrator.hpp"

#include "config.hpp"

#include <algorithm>
#include <stdexcept>

namespace knockthru {

Calibrator::Calibrator(KnockKind kind, const DetectorConfig& base, double start)
    : kind_(kind), config_(base), start_(start) {
    config_.controller_peak = std::min(config_.controller_peak, 30.0);
    config_.confirmed_peak = std::min(config_.confirmed_peak, 12.0);
    config_.partner_peak = std::min(config_.partner_peak, 8.0);
    config_.enabled = {true, true, true};
}

bool Calibrator::add(const Knock& knock) {
    if (knock.kind != kind_ || done()) return false;
    peaks_.push_back(knock.peak);
    return true;
}

double Calibrator::seconds_left(double now) const { return std::max(0.0, kSeconds - (now - start_)); }

double Calibrator::weakest() const {
    if (peaks_.empty()) throw std::logic_error("No knocks recorded");
    return *std::min_element(peaks_.begin(), peaks_.end());
}

double Calibrator::save() const {
    const double floor = kind_ == KnockKind::Surface ? 50.0 : 20.0;
    const double threshold = std::clamp(0.6 * weakest(), floor, 150.0);
    auto calibration = load_calibration();
    auto& slot = kind_ == KnockKind::Surface ? calibration.surface
        : kind_ == KnockKind::Together ? calibration.together : calibration.headset;
    slot = threshold;
    save_calibration(calibration);
    return threshold;
}

} // namespace knockthru
