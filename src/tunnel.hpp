#pragma once

/* A tunnel session's sockets: real UDP sockets on this host, used by a Rant node that runs
 * in the client (Rant built to wasm) as if it ran here. The bridge moves datagrams and never
 * reads them. Rant's spec/tunnel.md has the ops. */

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bridge {

/* Datagrams travel batched: [u8 9][u8 flags], then per datagram [u16 sock][4 ip][u16 port]
 * [4 via][u16 len][payload], the address bytes in network order, under kUdpBatch bytes. */
constexpr uint8_t kUdpOp = 9;
constexpr size_t  kUdpRecord = 14;
constexpr size_t  kUdpBatch = 60000;

class Tunnel {
public:
    /* A batch of datagrams for the client. Runs on the tunnel's own thread, once for what one
     * wake drained. */
    using Deliver = std::function<void(const std::string& batch)>;

    struct Iface { uint32_t addr, mask; };     /* network byte order */

    explicit Tunnel(Deliver deliver);
    ~Tunnel();
    Tunnel(const Tunnel&) = delete;
    Tunnel& operator=(const Tunnel&) = delete;

    /* Binds socket sock, made anew, to port on if_naddr (0 for every address). Returns the
     * bound port. Throws std::runtime_error with the reason. */
    uint16_t bind(uint16_t sock, uint32_t if_naddr, uint16_t port, bool reuse);
    /* Joins or leaves a multicast group. Throws std::runtime_error. */
    void membership(uint16_t sock, uint32_t group, uint32_t if_naddr, bool join);
    void ttl(uint16_t sock, int ttl);
    void loop(uint16_t sock, bool on);
    void own_groups(uint16_t sock);
    void close(uint16_t sock);

    /* Sends a datagram from the client. via is the outgoing interface for a group, 0 for the
     * default. False when the socket is unknown or the address is not one a node of this
     * host's LAN would use, so the bridge relays nothing beyond the mesh. */
    bool send(uint16_t sock, const uint8_t ip[4], uint16_t port, uint32_t via, const uint8_t* data, size_t n);

    /* This host's up IPv4 interfaces, loopback left out. */
    static std::vector<Iface> interfaces();

    static constexpr size_t kMaxSockets = 16;

private:
    struct Sock;
    void run();
    void wake();
    bool allowed(const uint8_t ip[4]) const;

    Deliver deliver_;
    std::mutex mu_;
    std::map<uint16_t, std::shared_ptr<Sock>> socks_;
    std::vector<Iface> ifaces_;
    intptr_t waker_ = -1;
    std::atomic<bool> stop_{ false };
    std::thread thread_;
};

}
