#pragma once

#include "knock_detector.hpp"
#include "pose_rate.hpp"

#include <openvr.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <ostream>
#include <thread>
#include <vector>

namespace knockthru {

constexpr std::array<const char*, 3> kDeviceNames{"hmd", "left", "right"}; // indexed by Device

Vec3 position_of(const vr::TrackedDevicePose_t& pose);
Vec3 velocity_of(const vr::TrackedDevicePose_t& pose);

constexpr const char* kCsvHeader = "t_s,device,valid,px,py,pz,vx,vy,vz,wx,wy,wz\n";

// One row of the tests/data CSV format. Without `angular`, its columns are zero.
void write_csv_row(
    std::ostream& out, double t, Device device, const Vec3& p, const Vec3& v, const float* angular = nullptr);

// Reads headset and controller poses once per call. SteamVR poses are only
// valid in the standing universe on the Frame.
class PoseSampler {
public:
    using Callback = std::function<void(Device, const vr::TrackedDevicePose_t&)>;

    void sample(const Callback& callback) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_refresh_) {
            auto* system = vr::VRSystem();
            indices_[0] = vr::k_unTrackedDeviceIndex_Hmd;
            indices_[1] = system->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
            indices_[2] = system->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
            next_refresh_ = now + std::chrono::seconds(1);
        }
        // Fetches only up to the highest device index in use, not all 64.
        std::uint32_t count = 0;
        for (const auto index : indices_)
            if (index < vr::k_unMaxTrackedDeviceCount) count = std::max(count, index + 1);
        vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(
            vr::TrackingUniverseStanding, 0, poses_.data(), count);
        for (std::size_t i = 0; i < 3; ++i) {
            if (indices_[i] >= vr::k_unMaxTrackedDeviceCount) continue;
            const auto& pose = poses_[indices_[i]];
            if (pose.bPoseIsValid) callback(static_cast<Device>(i), pose);
        }
    }

    static void feed(KnockDetector& detector, double t, Device device, const vr::TrackedDevicePose_t& pose) {
        detector.push(device, t, position_of(pose), velocity_of(pose));
    }

    static void write_csv(std::ostream& out, double t, Device device, const vr::TrackedDevicePose_t& pose) {
        write_csv_row(out, t, device, position_of(pose), velocity_of(pose), pose.vAngularVelocity.v);
    }

private:
    std::array<vr::TrackedDeviceIndex_t, 3> indices_{};
    std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> poses_{};
    std::chrono::steady_clock::time_point next_refresh_{};
};

class Clock {
public:
    double now() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    }
    // Sleeps until the next tick, `step` after the last one. After an overrun the
    // next tick is now, so missed ticks never run back to back.
    void tick(std::chrono::milliseconds step = std::chrono::milliseconds(1)) {
        next_ = std::max(next_ + step, std::chrono::steady_clock::now());
        std::this_thread::sleep_until(next_);
    }

private:
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_ = start_;
};

// Samples poses every millisecond on its own thread, or at PoseRate's idle rate
// while no controller is tracked. Knock strength is the velocity change between
// samples over the time between them, so a main loop busy drawing the panel
// would stretch those gaps and garble the strength.
class PoseThread {
public:
    struct Sample {
        Device device;
        double t;
        Vec3 position, velocity;
    };

    explicit PoseThread(const Clock& clock) : thread_([this, &clock] { loop(clock); }) {}
    ~PoseThread() {
        stop_ = true;
        thread_.join();
    }
    PoseThread(const PoseThread&) = delete;

    // Fills `samples` with the samples since the last call, oldest first. The two
    // buffers trade places, so both keep their capacity and neither allocates again.
    void take(std::vector<Sample>& samples) {
        samples.clear();
        std::lock_guard<std::mutex> lock(mutex_);
        samples.swap(samples_);
    }

private:
    void loop(const Clock& clock) {
        PoseSampler sampler;
        PoseRate rate;
        Clock ticker;
        while (!stop_) {
            const double t = clock.now();
            std::array<Sample, 3> read;
            std::size_t count = 0;
            sampler.sample([&](Device device, const vr::TrackedDevicePose_t& pose) {
                read[count++] = {device, t, position_of(pose), velocity_of(pose)};
            });
            {
                std::lock_guard<std::mutex> lock(mutex_);
                samples_.insert(samples_.end(), read.begin(), read.begin() + count);
            }
            const auto controllers = std::count_if(
                read.begin(), read.begin() + count, [](const Sample& s) { return s.device != Device::Hmd; });
            ticker.tick(rate.next(static_cast<int>(controllers)));
        }
    }

    std::mutex mutex_;
    std::vector<Sample> samples_;
    std::atomic<bool> stop_{false};
    std::thread thread_; // last, so it starts after the members it uses
};

} // namespace knockthru
