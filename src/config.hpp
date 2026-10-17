#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace bridge {

struct IceServer {
    std::string urls, username, credential;
};

/* The bridge settings, read from bridge.yaml. Every field has a working default. */
struct Config {
    uint16_t                 port = 7480;
    std::string              bind = "127.0.0.1";
    std::string              passphrase;            /* empty = no passphrase asked */
    std::vector<std::string> allowed_origins;       /* empty = any origin */
    std::vector<IceServer>   ice;
    bool                     webrtc = true;
    uint16_t                 rtc_port_lo = 0, rtc_port_hi = 0;
    uint32_t                 mtu = 1200;
    uint32_t                 grace_ms = 10000;
    uint32_t                 detach_ms = 3000;
    uint32_t                 max_connections = 32;
    uint64_t                 max_message_bytes = 64ull << 20;
    uint32_t                 queue_delay_warn_ms = 250;
};

/* $RANT_HOME, else ~/.rant. */
std::filesystem::path rant_home();
std::filesystem::path default_config_path();
std::filesystem::path state_path();

/* Throws std::runtime_error naming the file and line of the first problem. */
Config load_config(const std::filesystem::path& path);

/* Writes the commented default config with a fresh passphrase. false when it already exists. */
bool write_default_config(const std::filesystem::path& path);

}
