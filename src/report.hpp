#pragma once

#include <string>

namespace knockthru {

// What the panel's report link fills in on the GitHub issue form,
// .github/ISSUE_TEMPLATE/bug.yml.
struct IssueFields {
    std::string version, steamvr, passthrough;
    std::string error; // the last error KnockThru showed, empty for none
};

// Percent-encodes everything except letters, digits, and -._~.
std::string url_encode(const std::string& text);

// A link to a new GitHub issue with the bug form and `fields` filled in. Each
// field is cut to 100 characters, so the QR code stays coarse enough to scan
// through a headset lens.
std::string issue_url(const IssueFields& fields);

// Shortens a report to at most `limit` characters for a GitHub issue body,
// which GitHub caps at 65,536. Keeps the start, with the versions and
// settings, and the end, with the newest log lines, and marks the cut.
std::string fit_report(const std::string& report, std::size_t limit = 60000);

} // namespace knockthru
