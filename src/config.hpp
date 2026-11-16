#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "origin.hpp"

namespace bridge {

struct IceServer {
    std::string urls, username, credential;
};

constexpr uint16_t kDefaultPort = 6731;     /* Rant's ports are 6730 to 6739 */

/* The bridge settings, read from bridge.yaml. Every field has a working default. */
struct Config {
    uint16_t                 port = kDefaultPort;
    std::string              bind = "0.0.0.0";
    std::vector<std::string> allowed_origins = default_origins();   /* the pages that may open a session */
    std::vector<IceServer>   ice;
    bool                     webrtc = true;
    bool                     advertise = true;      /* answer rant-bridges.local and list the bridges, over mDNS */
    uint16_t                 rtc_port_lo = 0, rtc_port_hi = 0;
    uint32_t                 mtu = 1200;
    uint32_t                 grace_ms = 10000;
    uint32_t                 detach_ms = 3000;
    uint32_t                 max_connections = 32;
    uint64_t                 max_message_bytes = 1ull << 20;  /* the largest message a client may send */
    std::filesystem::path    path;                  /* the file it was read from */
};

/* $RANT_HOME, else ~/.rant. */
std::filesystem::path rant_home();
std::filesystem::path default_config_path();

/* Throws std::runtime_error naming the file and line of the first problem. */
Config load_config(const std::filesystem::path& path);

/* Writes the commented default config. false when it already exists. */
bool write_default_config(const std::filesystem::path& path);

/* Sets top level settings, writing the default config first when there is none. A key's line
   keeps its comment, a key the file lacks is added, and a result that would not load is undone. */
void set_config(const std::filesystem::path& path, const std::vector<std::pair<std::string, std::string>>& values);

}
