#pragma once

/* Finding bridges over mDNS: every bridge advertises _rant-bridge._tcp.local through DNS-SD,
 * answers the shared name rant-bridges.local with its own address, and browses for the
 * others, so a page that reaches any one of them learns them all. */

#include "net.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bridge {

/* The DNS messages mDNS carries, apart from the sockets so the unit test reaches them. */
namespace mdns {

constexpr const char* kSharedName = "rant-bridges.local";
constexpr const char* kService = "_rant-bridge._tcp.local";

enum Type : uint16_t { A = 1, PTR = 12, TXT = 16, SRV = 33, ANY = 255 };

struct Question {
    std::string name;
    uint16_t    type = 0;
    bool        unicast = false;      /* the asker wants the answer sent to it alone */
};

struct Record {
    std::string name;
    uint16_t    type = 0;
    uint32_t    ttl = 120;
    bool        flush = false;        /* a record only this host owns */
    uint32_t    ip = 0;               /* A, network byte order */
    std::string target;               /* PTR and SRV */
    uint16_t    port = 0;             /* SRV */
    std::vector<std::string> txt;     /* TXT */
};

struct Message {
    uint16_t id = 0;
    bool     response = false;
    std::vector<Question> questions;
    std::vector<Record>   answers, additional;
};

/* false for a message that does not parse whole. Names lose their trailing dot. */
bool parse(const uint8_t* p, size_t n, Message& out);
/* Names go out uncompressed. */
std::string build(const Message& m);
/* DNS names match without case. */
bool same_name(const std::string& a, const std::string& b);

}

/* A bridge on this network, this one among them. */
struct FoundBridge {
    std::string name;       /* the host it runs on */
    std::string address;    /* dotted IPv4 */
    uint16_t    port = 0;
    std::string version;
    bool        self = false;
};

class Finder {
public:
    /* Advertises a bridge on port to the interfaces given, loopback included when it is one of
     * them, and browses on them. Without the mDNS port it lists only this bridge. */
    Finder(std::vector<Iface> ifaces, uint16_t port, std::string version);
    ~Finder();
    Finder(const Finder&) = delete;
    Finder& operator=(const Finder&) = delete;

    std::vector<FoundBridge> bridges();

private:
    struct Peer {
        std::string target, name, version;
        uint16_t    port = 0;
        uint32_t    from = 0;          /* the address it answered from, when its A record is missing */
        std::chrono::steady_clock::time_point seen;
    };

    void run();
    void on_query(const mdns::Message& q, uint32_t from_ip, uint16_t from_port);
    void on_response(const mdns::Message& r, uint32_t from_ip);
    void send(const mdns::Message& m, uint32_t to_ip, uint16_t to_port, uint32_t via);
    void announce(uint32_t ttl);
    void browse();
    uint32_t local_for(uint32_t peer) const;
    void add_own(mdns::Message& m, uint32_t ip, uint32_t ttl, bool ptr, bool srv_txt, bool host) const;

    std::vector<Iface> ifaces_;
    uint16_t    port_;
    std::string version_, host_name_, instance_, host_;
    sock_t      fd_ = kBadSock;
    Waker       waker_;
    std::atomic<bool> stop_{ false };
    std::mutex  mu_;
    std::map<std::string, Peer> peers_;          /* by instance name */
    std::map<std::string, uint32_t> hosts_;      /* host name to address */
    std::thread thread_;
};

/* The interfaces a bridge bound to bind advertises on: every one for 0.0.0.0, else the one
 * it names, loopback included. */
std::vector<Iface> advertised_interfaces(const std::string& bind);

}
