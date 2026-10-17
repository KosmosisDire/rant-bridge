#pragma once

/* The data plane frame, one header for every op in both directions (PROTOCOL.md):
 * [u8 op][u8 flags][u16 id][u32 seq][u32 peer][u64 written_us][u64 capture_us][u8 text_len][text]
 * [u16 msg][u32 total][u32 offset] when flags bit 7 marks a chunk, then the payload. */

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace bridge {

enum Op : uint8_t { OpData = 1, OpVar = 2, OpCall = 3, OpResult = 4, OpProgress = 5, OpCancel = 6, OpPing = 7, OpPong = 8, OpUdp = 9 };

/* Tunnel datagrams travel batched: [u8 9][u8 flags], then per datagram
 * [u16 sock][4 ip][u16 port][4 via][u16 len][payload], the address bytes in network order.
 * A batch rides data channel 1 and stays under kUdpBatch bytes. */
constexpr size_t   kUdpRecord = 14;
constexpr size_t   kUdpBatch = 60000;
constexpr uint16_t kTunnelChannel = 1;
constexpr size_t   kTunnelBacklog = 4u << 20;     /* what may wait toward a client, as a socket buffer */

constexpr uint8_t kChunkFlag = 0x80;
constexpr size_t  kHeader = 29;
constexpr size_t  kChunkHeader = 10;

struct Frame {
    uint8_t  op = 0, flags = 0;
    uint16_t id = 0;
    uint32_t seq = 0, peer = 0;
    uint64_t written = 0, capture = 0;
    std::string_view text;
    bool     chunk = false;
    uint16_t msg = 0;
    uint32_t total = 0, offset = 0;
    std::string_view payload;
};

std::string build_frame(const Frame& f);
bool parse_frame(const uint8_t* p, size_t n, Frame& f);

/* Joins chunked frames per entity. A newer message on an entity drops an unfinished older one. */
class Reassembly {
public:
    /* true with whole set when f completes a message. Unchunked frames pass straight through. */
    bool add(const Frame& f, std::string& whole, uint64_t max_bytes);
private:
    struct Partial { uint16_t msg = 0; uint32_t total = 0, got = 0; std::string bytes; };
    std::map<uint16_t, Partial> parts_;
};

}
