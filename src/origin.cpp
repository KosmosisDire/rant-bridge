#include "origin.hpp"

#include <cctype>
#include <cstdio>

namespace bridge {

std::vector<std::string> default_origins() { return { "local", "https://explore.rantlib.dev" }; }

static std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static bool ends_with(const std::string& s, const char* tail) {
    std::string t(tail);
    return s.size() > t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

/* The host of an http or https origin, brackets kept on IPv6, else empty. */
static std::string host_of(const std::string& origin) {
    size_t at = origin.find("://");
    if (at == std::string::npos) return "";
    std::string scheme = origin.substr(0, at);
    if (scheme != "http" && scheme != "https") return "";
    std::string rest = origin.substr(at + 3);
    if (!rest.empty() && rest[0] == '[') {
        size_t end = rest.find(']');
        return end == std::string::npos ? "" : rest.substr(0, end + 1);
    }
    return rest.substr(0, rest.find(':'));
}

static bool private_ipv4(const std::string& h) {
    unsigned a, b, c, d;
    char tail;
    if (std::sscanf(h.c_str(), "%3u.%3u.%3u.%3u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
        return false;
    return a == 127 || a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) || (a == 169 && b == 254);
}

/* No public site serves one of these: reaching it takes the LAN already. */
static bool local_host(const std::string& h) {
    return h == "localhost" || h == "[::1]" || ends_with(h, ".localhost") || ends_with(h, ".local") || private_ipv4(h);
}

bool origin_allowed(const std::vector<std::string>& allowed, const std::string& origin) {
    std::string o = lower(origin);
    for (std::string a : allowed) {
        a = lower(a);
        while (!a.empty() && a.back() == '/') a.pop_back();
        if (a == "*" || a == o) return true;
        if (a == "local" && local_host(host_of(o))) return true;
    }
    return false;
}

}
