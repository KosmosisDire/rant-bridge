/* rant-bridge: the bridge between Rant and WebSocket and WebRTC clients. README.md says how
 * to run it, PROTOCOL.md what it speaks. */

#include "config.hpp"
#include "server.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace bridge;

static std::atomic<bool> g_stop{ false };

#ifdef _WIN32
/* rant bridge stop sets this event, since a windowless process gets no Ctrl-C */
static HANDLE g_stop_event = nullptr;
static const wchar_t* kStopEvent = L"Local\\rant-bridge-stop";

static BOOL WINAPI on_console(DWORD) { g_stop = true; return TRUE; }
static bool stop_requested() {
    return g_stop || (g_stop_event && WaitForSingleObject(g_stop_event, 0) == WAIT_OBJECT_0);
}
#else
static void on_signal(int) { g_stop = true; }
static bool stop_requested() { return g_stop.load(); }
#endif

static void usage() {
    std::printf("usage: rant-bridge [--config PATH] [--log-file PATH] [--supervise]\n"
                "       rant-bridge --init-config [--config PATH]\n"
                "       rant-bridge --version\n"
                "Serves Rant to WebSocket and WebRTC clients. The config defaults to %s.\n",
                default_config_path().string().c_str());
}

/* Output goes to the log file, else to the terminal that started us, if any. */
static void route_output(const std::string& log_file) {
    if (!log_file.empty()) {
        fs::path p(log_file);
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        if (fs::exists(p, ec) && fs::file_size(p, ec) > (10u << 20)) fs::rename(p, fs::path(log_file + ".1"), ec);
        std::freopen(log_file.c_str(), "a", stdout);
        std::freopen(log_file.c_str(), "a", stderr);
    }
#ifdef _WIN32
    else if (GetStdHandle(STD_ERROR_HANDLE) == nullptr && AttachConsole(ATTACH_PARENT_PROCESS)) {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }
#endif
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
}

static long self_pid() {
#ifdef _WIN32
    return (long)GetCurrentProcessId();
#else
    return (long)getpid();
#endif
}

/* The supervisor's pid when one runs us, so rant bridge stop ends the pair. */
static void write_state(const std::string& url) {
    const char* sup = std::getenv("RANT_BRIDGE_SUPERVISOR_PID");
    fs::path p = state_path();
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << "pid: " << (sup && *sup ? std::atol(sup) : self_pid()) << "\n"
        << "url: " << url << "\n"
        << "version: " << RANT_BRIDGE_VERSION << "\n"
        << "started: " << (long long)std::time(nullptr) << "\n";
}

static void remove_state() {
    std::error_code ec;
    fs::remove(state_path(), ec);
}

/* Runs the bridge as a child and starts it again when it fails, until a stop. */
static int supervise(const char* self, const std::vector<std::string>& args) {
    int delay_s = 1;
#ifdef _WIN32
    (void)self;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
    for (const auto& a : args) cmd += L" \"" + std::wstring(a.begin(), a.end()) + L"\"";
    SetEnvironmentVariableW(L"RANT_BRIDGE_SUPERVISOR_PID", std::to_wstring(GetCurrentProcessId()).c_str());
    /* ending the supervisor ends the child: the job kills it on close */
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    while (!stop_requested()) {
        STARTUPINFOW si{}; si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        std::wstring line = cmd;
        if (!CreateProcessW(nullptr, &line[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            std::fprintf(stderr, "rant-bridge: cannot start the bridge process (error %lu)\n", GetLastError());
            return 1;
        }
        AssignProcessToJobObject(job, pi.hProcess);
        HANDLE waits[2] = { pi.hProcess, g_stop_event };
        DWORD w = WaitForMultipleObjects(g_stop_event ? 2 : 1, waits, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0 + 1 && WaitForSingleObject(pi.hProcess, 5000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 0);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        if (stop_requested() || code == 0) break;
        std::fprintf(stderr, "rant-bridge: the bridge exited with %lu, starting it again in %d s\n", code, delay_s);
        for (int i = 0; i < delay_s * 10 && !stop_requested(); i++) Sleep(100);
        delay_s = delay_s < 10 ? delay_s * 2 : 10;
    }
    CloseHandle(job);
#else
    setenv("RANT_BRIDGE_SUPERVISOR_PID", std::to_string(getpid()).c_str(), 1);
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(self));
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    while (!stop_requested()) {
        pid_t child = fork();
        if (child == 0) { execvp(argv[0], argv.data()); _exit(127); }
        int status = 0;
        while (waitpid(child, &status, WNOHANG) == 0) {
            if (stop_requested()) { kill(child, SIGTERM); waitpid(child, &status, 0); break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (stop_requested() || (WIFEXITED(status) && WEXITSTATUS(status) == 0)) break;
        std::fprintf(stderr, "rant-bridge: the bridge exited, starting it again in %d s\n", delay_s);
        for (int i = 0; i < delay_s * 10 && !stop_requested(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        delay_s = delay_s < 10 ? delay_s * 2 : 10;
    }
#endif
    remove_state();
    return 0;
}

int main(int argc, char** argv) {
    std::string config = default_config_path().string(), log_file;
    bool init = false, sup = false;
    std::vector<std::string> child_args;    /* what a supervised child gets: everything but --supervise */
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        bool has_value = i + 1 < argc;
        if (a == "--config" && has_value) { config = argv[++i]; child_args.insert(child_args.end(), { a, config }); }
        else if (a == "--log-file" && has_value) { log_file = argv[++i]; child_args.insert(child_args.end(), { a, log_file }); }
        else if (a == "--supervise") sup = true;
        else if (a == "--init-config") init = true;
        else if (a == "--version") { std::printf("%s\n", RANT_BRIDGE_VERSION); return 0; }
        else { usage(); return a == "--help" || a == "-h" ? 0 : 2; }
    }
    if (init) {
        try {
            bool made = write_default_config(config);
            std::printf("%s\n", config.c_str());
            if (made) std::fprintf(stderr, "wrote a new config with a fresh passphrase\n");
            return 0;
        } catch (const std::exception& e) { std::fprintf(stderr, "rant-bridge: %s\n", e.what()); return 1; }
    }
    route_output(log_file);
#ifdef _WIN32
    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, kStopEvent);
    /* the top process clears a stop left over from before; a supervised child must not */
    const char* under = std::getenv("RANT_BRIDGE_SUPERVISOR_PID");
    if (g_stop_event && !(under && *under)) ResetEvent(g_stop_event);
    SetConsoleCtrlHandler(on_console, TRUE);
    /* 1 ms waits: the sender paces the WebSocket a chunk at a time, and 15 ms ticks would throttle it */
    timeBeginPeriod(1);
#else
    std::signal(SIGTERM, on_signal);
    std::signal(SIGINT, on_signal);
#endif
    if (sup) return supervise(argv[0], child_args);

    Config cfg;
    try { cfg = load_config(config); }
    catch (const std::exception& e) { std::fprintf(stderr, "rant-bridge: %s\n", e.what()); return 1; }
    int rc = serve(cfg, stop_requested, write_state);
    remove_state();
    return rc;
}
