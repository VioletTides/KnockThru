#include "report.hpp"

#include <cctype>
#include <algorithm>
#include <cstdio>

namespace knockthru {

std::string url_encode(const std::string& text) {
    std::string out;
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (std::isalnum(byte) || c == '-' || c == '.' || c == '_' || c == '~') {
            out += c;
        } else {
            char hex[4];
            std::snprintf(hex, sizeof hex, "%%%02X", byte);
            out += hex;
        }
    }
    return out;
}

std::string issue_url(const IssueFields& fields) {
    const auto field = [](const char* id, const std::string& value) {
        // Cuts on a character boundary, so UTF-8 stays valid.
        std::size_t end = std::min<std::size_t>(value.size(), 100);
        while (end < value.size() && end > 0 && (static_cast<unsigned char>(value[end]) & 0xC0) == 0x80)
            --end;
        return std::string("&") + id + "=" + url_encode(value.substr(0, end));
    };
    return "https://github.com/VioletTides/KnockThru/issues/new?template=bug.yml"
        + field("version", fields.version) + field("steamvr", fields.steamvr)
        + field("passthrough", fields.passthrough)
        + (fields.error.empty() ? "" : field("last-error", fields.error));
}

std::string fit_report(const std::string& report, std::size_t limit) {
    if (report.size() <= limit) return report;
    const std::size_t head = limit / 8;
    const std::string mark =
        "\n[... " + std::to_string(report.size() - limit) + " characters of older log lines cut ...]\n";
    // Both cuts land on line breaks, so no log line is split.
    std::size_t head_end = report.rfind('\n', head);
    if (head_end == std::string::npos) head_end = head;
    std::size_t tail_start = report.find('\n', report.size() - (limit - head_end - mark.size()));
    if (tail_start == std::string::npos) tail_start = report.size() - (limit - head_end - mark.size());
    return report.substr(0, head_end) + mark + report.substr(tail_start + 1);
}

} // namespace knockthru
