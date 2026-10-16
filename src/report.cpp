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
        while (end < value.size() && end > 0 && (static_cast<unsigned char>(value[end]) & 0xC0) == 0x80) --end;
        return std::string("&") + id + "=" + url_encode(value.substr(0, end));
    };
    return "https://github.com/VioletTides/KnockThru/issues/new?template=bug.yml" + field("version", fields.version) +
           field("steamvr", fields.steamvr) + field("passthrough", fields.passthrough);
}

} // namespace knockthru
