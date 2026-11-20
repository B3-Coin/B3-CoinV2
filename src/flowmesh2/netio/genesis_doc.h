// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_NETIO_GENESIS_DOC_H
#define B3_FLOWMESH2_NETIO_GENESIS_DOC_H

#include <flowmesh2/core/authority.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/market.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/seat_set.h>
#include <flowmesh2/net/tcp_mesh.h>
#include <flowmesh2/state/candidate.h>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/** The public genesis document of a FlowMesh V2 network (stage 2 step 5,
 * M5-NODE-01; O-105): everything a seat needs to build the genesis head and
 * its mesh, and everything a client needs to sign and to verify with
 * b3verify, in one JSON file every party reads. Nothing in it is secret.
 *
 *   {"format": "fm2-genesis/1",
 *    "network": {"chain": "regtest", "magic": hex4, "domain": hex32},
 *    "mesh_magic": hex4,
 *    "base": {"height": n, "hash": hex32}, "facts_start": {"height": n, "index": n},
 *    "seats": [{"index": i, "bls_pubkey": hex48, "pop": hex96,
 *               "mesh": {"address": ip, "port": n}, "api": {"address": ip, "port": n},
 *               "endpoint_record": {"transport_key": hex32, "valid_from_epoch": n,
 *                                   "valid_until_epoch": n, "counter": n, "signature": hex96}}],
 *    "assets": {"B3": hex32, "rUSD": hex32, ...},
 *    "markets": [{"base": name, "quote": name, "base_atoms_per_lot": n, "quote_atoms_per_tick": n}]}
 *
 * The seat set is SeatSet::FromPoP over the seats in index order (so seat i
 * is the i-th registration, the certificate bitmap order); the authority of
 * epoch 0 is {0, seats.Root(), ProfileHash(profile)}; the genesis head is
 * state::GenesisHead(profile, markets, DomainHash(network, FM_VERSION),
 * authority, base, facts_start): deterministic, so every seat and client
 * computes the same one. Markets are fixed at genesis (D6; listings are
 * M7-GOV-01) and name their assets by the table. REGTEST only until a
 * profile for another chain exists (GENESIS_NOT_REGTEST).
 */
namespace flowmesh2::netio {

inline constexpr std::string_view GENESIS_FORMAT{"fm2-genesis/1"};

inline constexpr std::string_view GENESIS_JSON{"genesis document is not the expected JSON"};          //!< (DECODE)
inline constexpr std::string_view GENESIS_FORMAT_UNKNOWN{"genesis document of another format"};      //!< (DECODE)
inline constexpr std::string_view GENESIS_NOT_REGTEST{"genesis document is not for regtest"};        //!< (UNSUPPORTED_NETWORK)
inline constexpr std::string_view GENESIS_HEX{"a genesis field is not hex of the expected length"};  //!< (DECODE)
inline constexpr std::string_view GENESIS_SEATS{"genesis seats are not 0..k-1 in order"};            //!< (DECODE)
inline constexpr std::string_view GENESIS_ASSET_UNKNOWN{"a genesis market names an asset outside the table"}; //!< (DECODE)
inline constexpr std::string_view GENESIS_SEAT_RANGE{"seat index outside the genesis seats"};        //!< (INVALID_INPUT)

struct GenesisSeat {
    uint32_t index{0};
    std::array<unsigned char, 48> bls_pubkey{};
    std::array<unsigned char, 96> pop{};
    std::string mesh_address{"127.0.0.1"};
    uint16_t mesh_port{net::DEFAULT_SEAT_MESH_PORT};
    std::string api_address{"127.0.0.1"};
    uint16_t api_port{0};
    net::SeatEndpointRecord endpoint; //!< seat, addresses and port repeat the mesh fields
    friend bool operator==(const GenesisSeat&, const GenesisSeat&) = default;
};

struct GenesisMarket {
    std::string base;
    std::string quote;
    uint64_t base_atoms_per_lot{0};
    uint64_t quote_atoms_per_tick{0};
    friend bool operator==(const GenesisMarket&, const GenesisMarket&) = default;
};

struct GenesisDoc {
    NetworkId network;
    std::array<uint8_t, 4> mesh_magic{};
    Anchor base;
    FactCursor facts_start;
    std::vector<GenesisSeat> seats;
    std::map<std::string, AssetId> assets;
    std::vector<GenesisMarket> markets;

    std::string Json() const;
    static Result<GenesisDoc> Parse(std::string_view json);

    uint256 Domain() const;
    Result<SeatSet> SeatSetOf() const;
    AuthorityVersion Authority(const ProfileParams& profile, const SeatSet& seats) const;
    Result<std::vector<MarketDescriptor>> Descriptors() const;
    Result<state::HeadView> GenesisHead(const ProfileParams& profile, const SeatSet& seats) const;
    std::vector<net::SeatEndpointRecord> Endpoints() const;
    std::optional<AssetId> Asset(std::string_view name) const;
    Result<MarketDescriptor> Market(std::string_view base, std::string_view quote) const;
};

} // namespace flowmesh2::netio

#endif // B3_FLOWMESH2_NETIO_GENESIS_DOC_H
