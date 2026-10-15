#include "check.hpp"

#include "config.hpp"

#include <filesystem>
#include <fstream>
#include <string>

using namespace bridge;
namespace fs = std::filesystem;

int main() {
    fs::path dir = fs::temp_directory_path() / "rant-bridge-config-test";
    fs::remove_all(dir);
    fs::path p = dir / "bridge.yaml";

    CHECK(write_default_config(p));
    CHECK(!write_default_config(p));       /* never overwrites */
    Config c = load_config(p);
    CHECK(c.port == 7480 && c.bind == "127.0.0.1" && c.passphrase.size() == 34 && c.ice.size() == 2);

    std::ofstream(p) << "port: 9000\nprot: 1\n";
    bool refused = false;
    try { load_config(p); } catch (const std::exception& e) { refused = std::string(e.what()).find("bridge.yaml:2: unknown setting 'prot'") != std::string::npos; }
    CHECK(refused);

    std::ofstream(p) << "ice:\n  - urls: turn:t.example.com:3478\n    username: u\n    credential: s\nrtc_ports: \"50000-50010\"\n";
    c = load_config(p);
    CHECK(c.ice.size() == 1 && c.ice[0].username == "u" && c.rtc_port_lo == 50000 && c.rtc_port_hi == 50010);
    fs::remove_all(dir);
    return finish();
}
