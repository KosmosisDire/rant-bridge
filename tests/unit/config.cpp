#include "check.hpp"

#include "config.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
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
    CHECK(c.port == 7480 && c.bind == "0.0.0.0" && c.passphrase.size() == 34 && c.ice.size() == 2);

    std::ofstream(p) << "port: 9000\nprot: 1\n";
    bool refused = false;
    try { load_config(p); } catch (const std::exception& e) { refused = std::string(e.what()).find("bridge.yaml:2: unknown setting 'prot'") != std::string::npos; }
    CHECK(refused);

    std::ofstream(p) << "ice:\n  - urls: turn:t.example.com:3478\n    username: u\n    credential: s\nrtc_ports: \"50000-50010\"\n";
    c = load_config(p);
    CHECK(c.ice.size() == 1 && c.ice[0].username == "u" && c.rtc_port_lo == 50000 && c.rtc_port_hi == 50010);

    /* set_config: a key's line in place with its comment, a missing key added, a bad value undone */
    std::ofstream(p) << "port: 7480        # the port\npassphrase: x\n";
    set_config(p, { { "port", "7999" }, { "bind", "127.0.0.1" } });
    std::ifstream in(p);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    CHECK(text == "port: 7999        # the port\npassphrase: x\nbind: 127.0.0.1\n");
    bool undone = false;
    try { set_config(p, { { "port", "not a port" } }); } catch (const std::exception&) { undone = true; }
    std::ifstream again(p);
    std::string after((std::istreambuf_iterator<char>(again)), std::istreambuf_iterator<char>());
    again.close();
    CHECK(undone && after == text);

    /* a # inside a quoted value or a word is not a comment, and an empty quoted passphrase means none */
    std::ofstream(p) << "passphrase: \"a #b\"   # secret\nbind: x#y\n";
    set_config(p, { { "passphrase", "\"\"" } });
    std::ifstream third(p);
    std::string emptied((std::istreambuf_iterator<char>(third)), std::istreambuf_iterator<char>());
    third.close();
    CHECK(emptied == "passphrase: \"\"       # secret\nbind: x#y\n");
    c = load_config(p);
    CHECK(c.passphrase.empty() && c.bind == "x#y");
    set_config(p, { { "passphrase", "\"q\\\"#1\"" } });
    CHECK(load_config(p).passphrase == "q\"#1");
    fs::remove_all(dir);
    return finish();
}
