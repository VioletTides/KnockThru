#pragma once

#include <openvr.h>

#include <chrono>
#include <thread>
#include <vector>

namespace knockthru {

// Short buzzes on both controllers. Uses SteamVR input actions when the action
// manifest is loaded, and the older direct pulse call otherwise.
class Haptics {
public:
    void use_actions(vr::VRActionSetHandle_t set, vr::VRActionHandle_t left, vr::VRActionHandle_t right) {
        active_.ulActionSet = set;
        left_ = left;
        right_ = right;
    }

    bool pending() const { return !pending_.empty(); }

    // Queues `count` pulses, 150 ms apart.
    void buzz(int count, double now) {
        for (int i = 0; i < count; ++i) pending_.push_back(now + 0.15 * i);
    }

    void update(double now) {
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (*it > now) {
                ++it;
                continue;
            }
            pulse();
            it = pending_.erase(it);
        }
    }

private:
    vr::VRActionHandle_t left_ = vr::k_ulInvalidActionHandle;
    vr::VRActionHandle_t right_ = vr::k_ulInvalidActionHandle;
    vr::VRActiveActionSet_t active_{};
    std::vector<double> pending_;

    void pulse() {
        if (left_ != vr::k_ulInvalidActionHandle) {
            // SteamVR acts on an action set only after UpdateActionState. KnockThru reads no
            // input, so it updates the set before each pulse, not on every loop.
            vr::VRInput()->UpdateActionState(&active_, sizeof active_, 1);
            for (const auto action : {left_, right_})
                vr::VRInput()->TriggerHapticVibrationAction(
                    action, 0, 0.07f, 160, 1.0f, vr::k_ulInvalidInputValueHandle);
            return;
        }
        auto* system = vr::VRSystem();
        for (int i = 0; i < 14; ++i) { // 14 x 5 ms
            for (const auto role : {vr::TrackedControllerRole_LeftHand, vr::TrackedControllerRole_RightHand})
                system->TriggerHapticPulse(system->GetTrackedDeviceIndexForControllerRole(role), 0, 3999);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
};

} // namespace knockthru
