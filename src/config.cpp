#include "config.hpp"

#include <yaml-cpp/yaml.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <system_error>

namespace bridge {

namespace fs = std::filesystem;

fs::path rant_home() {
    if (const char* h = std::getenv("RANT_HOME"); h && *h) return fs::path(h);
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    return fs::path(home ? home : ".") / ".rant";
}

fs::path default_config_path() { return rant_home() / "bridge.yaml"; }
fs::path state_path() { return rant_home() / "bridge.state"; }

/* 140 random bits as 28 base32 characters in groups of four, easy to read and type. */
static std::string new_passphrase() {
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    std::random_device rd;
    std::string out;
    for (int i = 0; i < 28; i++) {
        if (i && i % 4 == 0) out += '-';
        out += alphabet[rd() % 32];
    }
    return out;
}

bool write_default_config(const fs::path& path) {
    if (fs::exists(path)) return false;
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + path.string());
    out << "# Rant bridge settings. Apply a change with: rant bridge restart\n"
           "port: " << kDefaultPort << "\n"
           "bind: 0.0.0.0                 # 0.0.0.0 every interface, 127.0.0.1 this computer only\n"
           "passphrase: " << new_passphrase() << "\n"
           "allowed_origins: []           # browser origins allowed to connect, empty = any\n"
           "ice:\n"
           "  - urls: stun:stun.l.google.com:19302\n"
           "  - urls: stun:stun.cloudflare.com:3478\n"
           "# - urls: turn:turn.example.com:3478\n"
           "#   username: user\n"
           "#   credential: secret\n"
           "webrtc: true\n"
           "advertise: true               # answer rant-bridges.local and list the bridges on this network\n"
           "rtc_ports: \"\"                 # \"6740-6759\" pins WebRTC to a range, empty = any\n"
           "mtu: 1200\n"
           "grace_ms: 10000               # how long a dropped client may resume its session\n"
           "detach_ms: 3000               # a silent client counts as dropped after this\n"
           "max_connections: 32\n"
           "max_message_bytes: 1048576       # the largest message a client may send\n";
    out.close();
#ifndef _WIN32
    /* the passphrase is a secret. On Windows the profile folder is already private */
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
#endif
    return true;
}

/* Where a line's comment starts, or stop: a # after a space and outside quotes, as YAML reads it. */
static size_t comment_at(const std::string& text, size_t at, size_t stop) {
    char quote = 0;
    for (size_t i = at; i < stop; i++) {
        char c = text[i];
        if (quote) {
            if (c == '\\' && quote == '"') i++;
            else if (c == quote) quote = 0;
        }
        else if (c == '"' || c == '\'') quote = c;
        else if (c == '#' && i > at && (text[i - 1] == ' ' || text[i - 1] == '\t')) return i;
    }
    return stop;
}

void set_config(const fs::path& path, const std::vector<std::pair<std::string, std::string>>& values) {
    write_default_config(path);
    std::string before;
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("cannot read " + path.string());
        before.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    std::string text = before;
    for (const auto& [key, value] : values) {
        const std::string head = key + ":";
        bool found = false;
        for (size_t at = 0; at < text.size() && !found;) {
            size_t end = text.find('\n', at);
            if (end == std::string::npos) end = text.size();
            size_t stop = end > at && text[end - 1] == '\r' ? end - 1 : end;
            if (text.compare(at, head.size(), head) == 0) {
                /* the new value in the old one's width, so the comment stays in its column */
                size_t hash = comment_at(text, at + head.size(), stop);
                std::string comment = text.substr(hash, stop - hash);
                std::string set = head + " " + value;
                if (!comment.empty()) {
                    size_t width = hash - at;
                    set += std::string(width > set.size() ? width - set.size() : 1, ' ') + comment;
                }
                text.replace(at, stop - at, set);
                found = true;
            }
            at = end + 1;
        }
        if (!found) {
            const std::string nl = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
            text += (text.empty() || text.back() == '\n' ? "" : nl) + head + " " + value + nl;
        }
    }
    auto write = [&](const std::string& s) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write " + path.string());
        out << s;
    };
    write(text);
    try {
        load_config(path);
    } catch (...) {
        write(before);
        throw;
    }
}

[[noreturn]] static void fail(const fs::path& path, const YAML::Node& n, const std::string& why) {
    throw std::runtime_error(path.filename().string() + ":" + std::to_string(n.Mark().line + 1) + ": " + why);
}

template <class T> static T scalar(const fs::path& path, const std::string& key, const YAML::Node& n) {
    try { return n.as<T>(); }
    catch (const YAML::Exception&) { fail(path, n, "'" + key + "' has the wrong kind of value"); }
}

Config load_config(const fs::path& path) {
    Config c;
    YAML::Node root;
    try { root = YAML::LoadFile(path.string()); }
    catch (const YAML::BadFile&) { throw std::runtime_error("no config at " + path.string() + ", run rant-bridge --init-config"); }
    catch (const YAML::Exception& e) { throw std::runtime_error(path.filename().string() + ": " + e.what()); }
    if (root.IsNull()) return c;
    if (!root.IsMap()) fail(path, root, "the config must be a map of settings");

    for (const auto& kv : root) {
        const std::string key = kv.first.as<std::string>();
        const YAML::Node& v = kv.second;
        if (key == "port") c.port = scalar<uint16_t>(path, key, v);
        else if (key == "bind") c.bind = scalar<std::string>(path, key, v);
        else if (key == "passphrase") c.passphrase = v.IsNull() ? "" : scalar<std::string>(path, key, v);
        else if (key == "allowed_origins") {
            if (!v.IsNull() && !v.IsSequence()) fail(path, v, "'allowed_origins' must be a list");
            for (const auto& o : v) c.allowed_origins.push_back(scalar<std::string>(path, key, o));
        }
        else if (key == "ice") {
            if (!v.IsNull() && !v.IsSequence()) fail(path, v, "'ice' must be a list");
            for (const auto& s : v) {
                if (!s.IsMap() || !s["urls"]) fail(path, s, "each 'ice' entry needs 'urls'");
                IceServer srv;
                for (const auto& f : s) {
                    std::string k = f.first.as<std::string>();
                    if (k == "urls") srv.urls = scalar<std::string>(path, k, f.second);
                    else if (k == "username") srv.username = scalar<std::string>(path, k, f.second);
                    else if (k == "credential") srv.credential = scalar<std::string>(path, k, f.second);
                    else fail(path, f.first, "unknown 'ice' key '" + k + "'");
                }
                c.ice.push_back(srv);
            }
        }
        else if (key == "webrtc") c.webrtc = scalar<bool>(path, key, v);
        else if (key == "advertise") c.advertise = scalar<bool>(path, key, v);
        else if (key == "rtc_ports") {
            std::string r = v.IsNull() ? "" : scalar<std::string>(path, key, v);
            if (!r.empty()) {
                unsigned lo = 0, hi = 0;
                if (std::sscanf(r.c_str(), "%u-%u", &lo, &hi) != 2 || lo < 1 || hi < lo || hi > 65535)
                    fail(path, v, "'rtc_ports' wants a range like 6740-6759");
                c.rtc_port_lo = (uint16_t)lo; c.rtc_port_hi = (uint16_t)hi;
            }
        }
        else if (key == "mtu") {
            c.mtu = scalar<uint32_t>(path, key, v);
            if (c.mtu < 576 || c.mtu > 9000) fail(path, v, "'mtu' wants 576 to 9000");
        }
        else if (key == "grace_ms") c.grace_ms = scalar<uint32_t>(path, key, v);
        else if (key == "detach_ms") c.detach_ms = scalar<uint32_t>(path, key, v);
        else if (key == "max_connections") c.max_connections = scalar<uint32_t>(path, key, v);
        else if (key == "max_message_bytes") c.max_message_bytes = scalar<uint64_t>(path, key, v);
        /* a setting of the per client nodes, gone with them: a config written before still loads */
        else if (key == "queue_delay_warn_ms")
            std::fprintf(stderr, "rant-bridge: %s: 'queue_delay_warn_ms' is no longer used, remove it\n", path.filename().string().c_str());
        else fail(path, kv.first, "unknown setting '" + key + "'");
    }
    return c;
}

}
