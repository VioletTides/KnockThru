#include "knock_actions.hpp"

namespace knockthru {
namespace {

int count_index(int count) {
    for (std::size_t i = 0; i < kKnockCounts.size(); ++i)
        if (kKnockCounts[i] == count) return static_cast<int>(i);
    return -1;
}

} // namespace

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

const char* count_word(int count) {
    if (count == 2) return "twice";
    if (count == 3) return "thrice";
    return "more";
}

KnockAction KnockActions::default_for(int count) {
    if (count == 2) return KnockAction::Passthrough;
    if (count == 3) return KnockAction::Style;
    return KnockAction::None;
}

KnockActions::Row KnockActions::defaults() {
    Row row{};
    for (std::size_t i = 0; i < kKnockCounts.size(); ++i) row[i] = default_for(kKnockCounts[i]);
    return row;
}

KnockAction KnockActions::get(KnockKind kind, int count) const {
    const int i = count_index(count);
    return i < 0 ? KnockAction::None : table_[static_cast<int>(kind)][i];
}

void KnockActions::set(KnockKind kind, int count, KnockAction action) {
    const int i = count_index(count);
    if (i >= 0) table_[static_cast<int>(kind)][i] = action;
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
