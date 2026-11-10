#pragma once

#include "knock_detector.hpp"

#include <array>
#include <optional>
#include <string>

namespace knockthru {

enum class KnockAction { Passthrough, Style, PerfGraph, None };
constexpr std::array<KnockAction, 4> kKnockActionList{
    KnockAction::Passthrough, KnockAction::Style, KnockAction::PerfGraph, KnockAction::None};

// The knock counts a person can give an action to: twice and thrice.
constexpr std::array<int, 2> kKnockCounts{2, 3};

// The name in config.json: passthrough, style, perf_graph, or none.
const char* to_string(KnockAction action);
std::optional<KnockAction> parse_knock_action(const std::string& name);

// "twice" for 2 and "thrice" for 3, for the panel. The detector reports no other count.
const char* count_word(int count);

// The action for each knock kind and count. Defaults: twice switches
// passthrough, thrice switches the style.
class KnockActions {
public:
    // `count` is one of kKnockCounts.
    static KnockAction default_for(int count);

    // A count outside kKnockCounts gives None.
    KnockAction get(KnockKind kind, int count) const;
    void set(KnockKind kind, int count, KnockAction action);
    void set_all(int count, KnockAction action);
    // The action every kind shares for `count`, or nothing when they differ.
    std::optional<KnockAction> shared(int count) const;
    bool is_default() const;

    bool operator==(const KnockActions& other) const { return table_ == other.table_; }

private:
    using Row = std::array<KnockAction, 2>; // for twice and thrice
    static constexpr Row kDefaults{KnockAction::Passthrough, KnockAction::Style};
    std::array<Row, 3> table_ = {kDefaults, kDefaults, kDefaults}; // indexed by KnockKind
};

} // namespace knockthru
