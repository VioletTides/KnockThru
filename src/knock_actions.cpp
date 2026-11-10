#include "knock_actions.hpp"

namespace knockthru {

const char* to_string(KnockAction action) {
    switch (action) {
    case KnockAction::Passthrough: return "passthrough";
    case KnockAction::Style: return "style";
    case KnockAction::PerfGraph: return "perf_graph";
    case KnockAction::None: return "none";
    }
    return "none";
}

std::optional<KnockAction> parse_knock_action(const std::string& name) {
    for (const auto action : kKnockActionList)
        if (name == to_string(action)) return action;
    return std::nullopt;
}

const char* count_word(int count) { return count == 2 ? "twice" : "thrice"; }

KnockAction KnockActions::default_for(int count) { return kDefaults[count - 2]; }

KnockAction KnockActions::get(KnockKind kind, int count) const {
    return count == 2 || count == 3 ? table_[static_cast<int>(kind)][count - 2] : KnockAction::None;
}

void KnockActions::set(KnockKind kind, int count, KnockAction action) {
    if (count == 2 || count == 3) table_[static_cast<int>(kind)][count - 2] = action;
}

void KnockActions::set_all(int count, KnockAction action) {
    for (int k = 0; k < 3; ++k) set(static_cast<KnockKind>(k), count, action);
}

std::optional<KnockAction> KnockActions::shared(int count) const {
    const auto first = get(KnockKind::Together, count);
    for (const auto kind : {KnockKind::Surface, KnockKind::Headset})
        if (get(kind, count) != first) return std::nullopt;
    return first;
}

bool KnockActions::is_default() const { return *this == KnockActions{}; }

} // namespace knockthru
