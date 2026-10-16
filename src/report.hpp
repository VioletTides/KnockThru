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

} // namespace knockthru
