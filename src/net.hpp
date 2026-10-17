#pragma once

/* The socket calls the tunnel and the bridge finder share, over Winsock or POSIX. */

#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#endif

namespace bridge {

#ifdef _WIN32
using sock_t = SOCKET;
constexpr sock_t kBadSock = INVALID_SOCKET;
#else
using sock_t = int;
constexpr sock_t kBadSock = -1;
#endif

int close_sock(sock_t s);
void nonblock(sock_t s);
int poll_socks(pollfd* fds, size_t n, int timeout_ms);
/* "what failed (the OS error)" */
std::string failed(const char* what);

/* An up IPv4 interface of this host, in network byte order. */
struct Iface { uint32_t addr, mask; };
/* Loopback left out. */
std::vector<Iface> local_interfaces();

/* A loopback socket connected to itself: a send to it ends a poll early. */
class Waker {
public:
    Waker();
    ~Waker();
    Waker(const Waker&) = delete;
    Waker& operator=(const Waker&) = delete;
    bool ok() const { return fd_ != kBadSock; }
    sock_t fd() const { return fd_; }
    void wake();
    void drain();
private:
    sock_t fd_ = kBadSock;
};

}
