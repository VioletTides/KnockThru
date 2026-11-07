// What's new tests: whats-new.md parses, has the current version, and fits the screen, and the update
// card's What's new button opens pages that Newer and Older step through.
#include "check.hpp"
#include "panel.hpp"
#include "update.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using namespace knockthru;

namespace {

void parse_tests() {
    std::cout << "parse\n";
    const auto versions =
        parse_whats_new("# What's new\nIntro\n\n## 1.1.0\n- Two\r\n- Three\n## 1.0.0\n- One\n");
    check(versions.size() == 2 && versions[0].version == "1.1.0" && versions[1].notes.size() == 1,
        "versions and notes come out in file order");
    check(versions[0].notes[0] == "Two", "a Windows line end is dropped");

    const auto all = parse_whats_new("## 1.3.0\n- c\n## 1.2.0\n- b\n## 1.1.0\n- a\n## 1.0.0\n- first\n");
    const auto since = notes_between(all, "1.1.0", "1.2.0");
    check(since.size() == 1 && since[0].version == "1.2.0", "a newer unreleased version is left out");
    check(notes_between(all, "", "1.3.0").size() == 4, "an unknown old version gets every version");
    check(notes_between(all, "1.3.0", "1.3.0").empty(), "the same version gets nothing");
}

void file_tests() {
    std::cout << "whats-new.md\n";
    std::ifstream file(KNOCKTHRU_WHATS_NEW);
    const auto versions = parse_whats_new({std::istreambuf_iterator<char>(file), {}});
    bool current = false;
    for (const auto& version : versions)
        current = current || (version.version == KNOCKTHRU_VERSION && !version.notes.empty());
    check(current, std::string("has notes for version ") + KNOCKTHRU_VERSION);
    Panel panel(KNOCKTHRU_FONTS);
    const auto overflow = panel.whats_new_overflow(versions);
    check(overflow.empty(), "every note and version fits" + (overflow.empty() ? "" : ": " + overflow));
    std::string long_note;
    for (int i = 0; i < 40; ++i) long_note += "knock ";
    check(!panel.whats_new_overflow({{"9.9.9", {long_note}}}).empty(), "a note over two lines is caught");
    check(
        !panel.whats_new_overflow({{"9.9.9", {std::string(200, 'x')}}}).empty(), "a too wide word is caught");
}

void page_tests() {
    std::cout << "pages\n";
    PanelState state;
    for (int i = 12; i > 0; --i)
        state.whats_new.push_back({"1." + std::to_string(i) + ".0", {"Something changed", "Something else"}});
    state.update_notice = UpdateNotice{false, "You now have version 1.12.0."};
    Panel panel(KNOCKTHRU_FONTS);
    const auto pages = panel.whats_new_pages(state.whats_new);
    check(pages.size() > 1 && pages[0] == 0, "twelve versions take more than one page");

    panel.render(state);
    const auto clicked = panel.click(590, 466); // What's new, left of OK
    check(clicked && clicked->command == PanelCommand::DismissUpdateNotice, "What's new dismisses the card");
    state.update_notice.reset();
    const auto first = panel.render(state).hash();
    check(!panel.click(870, 68), "Newer does nothing on the first page");
    check(panel.click(1060, 68).has_value(), "Older responds on the first page");
    const auto second = panel.render(state).hash();
    check(second != first, "Older shows the next page");
    for (std::size_t i = 2; i < pages.size(); ++i) {
        panel.click(1060, 68);
        panel.render(state);
    }
    check(!panel.click(1060, 68), "Older does nothing on the last page");
    panel.click(870, 68);
    panel.render(state);
    for (std::size_t i = 2; i < pages.size(); ++i) {
        panel.click(870, 68);
        panel.render(state);
    }
    check(panel.render(state).hash() == first, "Newer steps back to the first page");

    // One version whose notes fit shows them on the card, with no What's new button.
    PanelState one;
    one.whats_new = {{"1.1.0", {"Something changed"}}};
    one.update_notice = UpdateNotice{false, "You now have version 1.1.0."};
    Panel card(KNOCKTHRU_FONTS);
    card.render(one);
    check(!card.click(590, 486), "one short version has no What's new button");
}

} // namespace

int main() {
    parse_tests();
    file_tests();
    page_tests();
    return finish();
}
