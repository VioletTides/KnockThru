#pragma once

#include <string>

namespace knockthru {

// What the panel's report link fills in on the GitHub issue form,
// .github/ISSUE_TEMPLATE/bug.yml.
struct IssueFields {
    std::string version, steamvr, passthrough;
    std::string error; // the last error KnockThru showed, empty for none
};

// A link to a new GitHub issue with the bug form and `fields` filled in. Each
// field is cut to 100 characters, so the QR code stays coarse enough to scan
// through a headset lens.
std::string issue_url(const IssueFields& fields);

// Shortens a report to at most 60,000 characters for a GitHub issue body,
// which GitHub caps at 65,536. Keeps the start, with the versions and
// settings, and the end, with the newest log lines, and marks the cut.
std::string fit_report(const std::string& report);

} // namespace knockthru
