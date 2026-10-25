// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <flowmesh2/net/tcp_mesh_wire.h>

#include <algorithm>

namespace flowmesh2::net {
namespace {
void Put32(unsigned char* p, uint32_t n)
{
    for (int i{0}; i != 4; ++i) p[i] = (n >> (8 * i)) & 255;
}
uint32_t Get32(const unsigned char* p)
{
    uint32_t n{0};
    for (int i{0}; i != 4; ++i) n |= uint32_t{p[i]} << (8 * i);
    return n;
}
} // namespace

std::array<unsigned char, HELLO_BYTES> EncodeHello(const MeshHello& hello)
{
    std::array<unsigned char, HELLO_BYTES> out{};
    out[0] = MESH_WIRE_VERSION;
    out[1] = static_cast<unsigned char>(hello.lane);
    Put32(out.data() + 2, hello.from_seat);
    Put32(out.data() + 6, hello.to_seat);
    std::copy(hello.ellswift.begin(), hello.ellswift.end(), out.begin() + 10);
    return out;
}

std::optional<MeshHello> CheckHello(std::span<const unsigned char, HELLO_BYTES> bytes, const HelloExpectation& expect)
{
    MeshHello hello;
    hello.lane = static_cast<Lane>(bytes[1]);
    hello.from_seat = Get32(bytes.data() + 2);
    hello.to_seat = Get32(bytes.data() + 6);
    std::copy(bytes.begin() + 10, bytes.end(), hello.ellswift.begin());
    if (bytes[0] != MESH_WIRE_VERSION || !IsKnown(hello.lane) || hello.to_seat != expect.self_seat ||
        hello.from_seat >= expect.seat_count || hello.from_seat == expect.self_seat ||
        (expect.initiator && (hello.from_seat != expect.peer || hello.lane != expect.lane)) ||
        (!expect.initiator && hello.from_seat >= expect.self_seat)) {
        return std::nullopt;
    }
    return hello;
}

} // namespace flowmesh2::net
