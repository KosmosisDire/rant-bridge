/* rant-bridge: lends this host's UDP sockets to Rant nodes that run in pages, over WebSocket
 * and WebRTC. README.md says how to run it, Rant's spec/tunnel.md what it speaks. */

#include "config.hpp"
#include "server.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace bridge;

static std::atomic<bool> g_stop{ false };

static bool stop_requested() { return g_stop.load(); }

#ifdef _WIN32
static BOOL WINAPI on_console(DWORD) { g_stop = true; return TRUE; }

static LRESULT CALLBACK on_window(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLOSE) {
        g_stop = true;
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

/* rant stop, as taskkill does, asks a program with no console to close its windows, so a
 * hidden one stands for the bridge */
static void close_on_window() {
    std::thread([] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = on_window;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"rant-bridge";
        RegisterClassW(&wc);
        if (!CreateWindowExW(0, wc.lpszClassName, L"rant-bridge", 0, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr)) return;
        MSG m;
        while (GetMessageW(&m, nullptr, 0, 0) > 0) DispatchMessageW(&m);
    }).detach();
}
#else
static void on_signal(int) { g_stop = true; }
#endif

static void usage() {
    std::printf("usage: rant-bridge [--config PATH]\n"
                "       rant-bridge --init-config [--config PATH] [--bind IP] [--port N] [--pass TEXT]\n"
                "       rant-bridge --version\n"
                "Lends this host's UDP sockets to Rant nodes elsewhere. The config defaults to %s,\n"
                "written when missing by a start or --init-config. --bind and --port set where it\n"
                "listens and --pass sets the passphrase, empty for none. A running bridge takes them at\n"
                "its next start.\n",
                default_config_path().string().c_str());
}

/* A dotted IPv4 address, such as 0.0.0.0 for every interface. */
static bool dotted_ipv4(const std::string& s) {
    int parts = 0;
    for (size_t at = 0; at <= s.size(); parts++) {
        size_t dot = s.find('.', at);
        if (dot == std::string::npos) dot = s.size();
        std::string part = s.substr(at, dot - at);
        if (part.empty() || part.size() > 3 || part.find_first_not_of("0123456789") != std::string::npos ||
            std::atoi(part.c_str()) > 255)
            return false;
        at = dot + 1;
    }
    return parts == 4;
}

/* A YAML double quoted scalar, so any printable text survives as the passphrase. */
static bool yaml_quoted(const std::string& s, std::string& out) {
    out = "\"";
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7f) return false;
        if (c == '"' || c == '\\') out += '\\';
        out += (char)c;
    }
    out += '"';
    return true;
}

/* Output goes where the one that started us sent it, rant to the node's log, else to the
 * terminal, which a windowless program has to attach to. */
static void route_output() {
#ifdef _WIN32
    if (GetStdHandle(STD_ERROR_HANDLE) == nullptr && AttachConsole(ATTACH_PARENT_PROCESS)) {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }
#endif
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
}

int main(int argc, char** argv) {
    std::string config = default_config_path().string(), bind, port, pass;
    bool init = false, has_pass = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        bool has_value = i + 1 < argc;
        if (a == "--config" && has_value) config = argv[++i];
        else if (a == "--init-config") init = true;
        else if (a == "--bind" && has_value) bind = argv[++i];
        else if (a == "--port" && has_value) port = argv[++i];
        else if (a == "--pass" && has_value) { pass = argv[++i]; has_pass = true; }
        else if (a == "--version") { std::printf("%s\n", RANT_BRIDGE_VERSION); return 0; }
        else { usage(); return a == "--help" || a == "-h" ? 0 : 2; }
    }
    if ((!bind.empty() || !port.empty() || has_pass) && !init) { usage(); return 2; }
    if (init) {
        std::vector<std::pair<std::string, std::string>> set;
        if (!bind.empty()) {
            if (!dotted_ipv4(bind)) { std::fprintf(stderr, "rant-bridge: --bind takes an address such as 0.0.0.0 or 127.0.0.1\n"); return 2; }
            set.push_back({ "bind", bind });
        }
        if (!port.empty()) {
            long p = port.find_first_not_of("0123456789") == std::string::npos && port.size() <= 5 ? std::atol(port.c_str()) : 0;
            if (p < 1 || p > 65535) { std::fprintf(stderr, "rant-bridge: --port takes a number from 1 to 65535\n"); return 2; }
            set.push_back({ "port", std::to_string(p) });
        }
        if (has_pass) {
            std::string quoted;
            if (!yaml_quoted(pass, quoted)) { std::fprintf(stderr, "rant-bridge: --pass takes one line of printable text\n"); return 2; }
            set.push_back({ "passphrase", quoted });
        }
        try {
            bool made = !fs::exists(config);
            if (set.empty()) write_default_config(config);
            else set_config(config, set);
            std::printf("%s\n", config.c_str());
            if (made && !has_pass) std::fprintf(stderr, "wrote a new config with a fresh passphrase\n");
            else if (made) std::fprintf(stderr, "wrote a new config\n");
            for (const auto& [key, value] : set) {
                if (key != "passphrase") std::fprintf(stderr, "set %s to %s\n", key.c_str(), value.c_str());
                else if (pass.empty()) std::fprintf(stderr, "removed the passphrase: anyone who reaches the port can use the mesh\n");
                else std::fprintf(stderr, "set the passphrase\n");
            }
            return 0;
        } catch (const std::exception& e) { std::fprintf(stderr, "rant-bridge: %s\n", e.what()); return 1; }
    }
    route_output();
#ifdef _WIN32
    SetConsoleCtrlHandler(on_console, TRUE);
    close_on_window();
#else
    std::signal(SIGTERM, on_signal);
    std::signal(SIGINT, on_signal);
#endif

    Config cfg;
    try {
        /* an installed bridge has no setup step, so the first start writes the defaults */
        if (write_default_config(config)) std::fprintf(stderr, "wrote a new config with a fresh passphrase at %s\n", config.c_str());
        cfg = load_config(config);
    } catch (const std::exception& e) { std::fprintf(stderr, "rant-bridge: %s\n", e.what()); return 1; }
    return serve(cfg, stop_requested);
}
