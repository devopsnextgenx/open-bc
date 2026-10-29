// release_logic.h
// ---------------------------------------------------------------------------
// Qt-free decision logic for the Home tab's release notices (see update_notice.h):
//   * "What's new"       - the change log of the version that is running, shown ONCE after it was
//                          installed (until a different version is installed).
//   * "Update available" - every published release newer than the running one, newest first,
//                          unless the user chose "Don't notify for this release" for the newest.
// Pure functions over plain data so they are unit-tested without Qt:
//   g++ -std=c++17 -Wall -Wextra -I cpp cpp/tools/update_notice_test/update_notice_test.cpp -o update_notice_test
// ---------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace openbc::app::updates {

struct Release {
    std::string tag;        // as published, e.g. "v1.0.23"
    std::string title;      // release name (may be empty)
    std::string body;       // markdown change log
    std::string url;        // html page of the release
    std::string published;  // ISO date, display only
};

// "v1.0.23-beta.1" -> {1, 0, 23}. Leading 'v'/'V' and everything after the first character that is
// neither a digit nor a '.' is ignored. No digits at all -> empty vector (never newer, never equal).
inline std::vector<int> parseVersion(const std::string& text) {
    std::vector<int> parts;
    std::size_t i = (!text.empty() && (text[0] == 'v' || text[0] == 'V')) ? 1 : 0;
    long current = -1;
    for (; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (std::isdigit(c)) {
            current = (current < 0 ? 0 : current) * 10 + (c - '0');
            if (current > 1000000) current = 1000000;  // absurd component: clamp, never overflow
        } else if (c == '.') {
            if (current < 0) break;  // "1..2" / ".1": malformed, stop
            parts.push_back(static_cast<int>(current));
            current = -1;
        } else {
            break;
        }
    }
    if (current >= 0) parts.push_back(static_cast<int>(current));
    return parts;
}

// <0, 0, >0 like strcmp. Missing trailing components count as 0 ("1.0" == "1.0.0").
inline int compareVersions(const std::string& a, const std::string& b) {
    const std::vector<int> x = parseVersion(a), y = parseVersion(b);
    const std::size_t n = std::max(x.size(), y.size());
    for (std::size_t i = 0; i < n; ++i) {
        const int l = i < x.size() ? x[i] : 0, r = i < y.size() ? y[i] : 0;
        if (l != r) return l < r ? -1 : 1;
    }
    return 0;
}

inline bool isParsable(const Release& r) { return !parseVersion(r.tag).empty(); }

struct Plan {
    bool showWhatsNew = false;
    Release whatsNew;              // the release that matches the running version
    bool showUpdate = false;
    std::vector<Release> newer;    // newest first; the whole change log between "now" and "latest"
};

// `lastSeen`: version whose change log was already shown ("" = never). `skipped`: tag the user
// asked not to be notified about ("" = none). Releases whose tag has no version are ignored.
inline Plan plan(std::vector<Release> releases, const std::string& current, const std::string& lastSeen,
                 const std::string& skipped) {
    releases.erase(std::remove_if(releases.begin(), releases.end(), [](const Release& r) { return !isParsable(r); }),
                   releases.end());
    std::stable_sort(releases.begin(), releases.end(),
                     [](const Release& a, const Release& b) { return compareVersions(a.tag, b.tag) > 0; });
    Plan out;
    const bool seen = !lastSeen.empty() && compareVersions(lastSeen, current) == 0;
    for (const Release& r : releases) {
        const int cmp = compareVersions(r.tag, current);
        if (cmp > 0) out.newer.push_back(r);
        else if (cmp == 0 && !seen && !out.showWhatsNew) { out.showWhatsNew = true; out.whatsNew = r; }
    }
    out.showUpdate = !out.newer.empty() && out.newer.front().tag != skipped;
    return out;
}

}  // namespace openbc::app::updates
