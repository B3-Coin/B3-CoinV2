// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_NET_SHORT_ID_H
#define B3_FLOWMESH2_NET_SHORT_ID_H

#include <flowmesh2/agreement/messages.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/result.h>
#include <uint256.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

/** Salted short IDs of actions for compact relay (M4-NET-02; BIP152
 * pattern; research-network-api section 5; PLAN-10).
 *
 * Keys (O-97, PROVISIONAL REGTEST): the digest
 * Tagged(SHORT_ID_KEY, header_hash || u64 LE nonce); k0 and k1 are its
 * bytes 0..7 and 8..15 read as u64 LE. BIP152 takes one SHA256 of the
 * block header and the nonce; fm2 hashes the header hash under its own tag
 * (core/hashes.h), so a short-ID key is never another fm2 digest.
 * Encode(id) is the 6 least significant bytes, little-endian, of
 * SipHash-2-4(k0, k1) over the ActionId's 32 bytes (crypto/siphash.h).
 * contrib reference: ~/B3-evidence/M4/M4-NET-02/scripts/shortid_ref.py.
 *
 * Short IDs only shrink relay encoding. They "are expected to occasionally
 * collide" (BIP152): what is signed and certified always carries the full
 * 32-byte ActionIds, and nothing here decides what a slot contains.
 *
 * Body fetch. Resolve matches a proposal's short IDs against the actions a
 * seat holds. A position is resolved when exactly one held action has its
 * short ID and no other position of the proposal repeats it. Otherwise it
 * is open: MISSING (no held action: fetch the body) or AMBIGUOUS (two or
 * more held actions, or the short ID repeats within the proposal: fetch the
 * full ID). Fill closes an open position with a fetched ActionId whose
 * short ID is the position's. It cannot tell two colliding actions apart:
 * the source of a fetched ID is trusted here, and the body a seat
 * assembles is checked against the header (actions_root) before any vote. The fetch messages and their lane (BULK)
 * belong to the mesh and the agreement (M4-AGR-02, M4-AGR-04); this codec
 * neither sends nor waits. The inclusion-list IDs of agreement/messages.h
 * are meant to use the same codec (M4-AGR-04, not wired here).
 *
 * Deterministic; no lock, clock or I/O. Resolve costs
 * O((h + s) log h) for h held actions and s short IDs.
 */
namespace flowmesh2::net {

//! Why a position of a Resolution is open.
enum class ShortIdOpen : uint8_t {
    MISSING = 1,   //!< no held action has this short ID: fetch the body
    AMBIGUOUS = 2, //!< two or more candidates: fetch the full ID
};

//! A Fill refusal (ErrorCode::INVALID_INPUT).
inline constexpr std::string_view SHORT_ID_FILL_INDEX{"fill index outside the proposal"};
inline constexpr std::string_view SHORT_ID_FILL_RESOLVED{"fill of a resolved position"};
inline constexpr std::string_view SHORT_ID_FILL_MISMATCH{"fetched action has another short ID"};

//! A proposal's short IDs matched against the held actions (see the file comment).
struct ShortIdResolution {
    std::vector<agreement::ShortId> shorts;     //!< the proposal's short IDs, in order
    std::vector<std::optional<ActionId>> ids;   //!< one per short ID; nullopt while open
    std::vector<std::optional<ShortIdOpen>> open; //!< one per short ID; set while open

    //! The open positions, ascending: the fetch list.
    std::vector<uint32_t> Open() const;
    //! True when every position is resolved.
    bool Complete() const;
    //! The full IDs in proposal order; nullopt unless Complete().
    std::optional<std::vector<ActionId>> Ids() const;
};

class ShortIdCodec
{
public:
    //! A codec keyed by one proposal (see the file comment).
    ShortIdCodec(const uint256& header_hash, uint64_t nonce);

    //! The short ID of `id`. Never fails (Result for the stub's interface).
    Result<agreement::ShortId> Encode(const ActionId& id) const;
    //! Every distinct ActionId of `known` whose short ID is `short_id`, ascending. Never fails.
    Result<std::vector<ActionId>> Decode(const agreement::ShortId& short_id, std::span<const ActionId> known) const;
    //! Match `shorts` against `held` (duplicates in `held` count once).
    ShortIdResolution Resolve(std::span<const agreement::ShortId> shorts, std::span<const ActionId> held) const;
    //! Close open position `index` of `resolution` with a fetched ActionId.
    Result<void> Fill(ShortIdResolution& resolution, uint32_t index, const ActionId& fetched) const;

    const uint256& HeaderHash() const { return m_header_hash; }
    uint64_t Nonce() const { return m_nonce; }
    uint64_t K0() const { return m_k0; }
    uint64_t K1() const { return m_k1; }

private:
    agreement::ShortId ShortOf(const ActionId& id) const;

    uint256 m_header_hash;
    uint64_t m_nonce;
    uint64_t m_k0;
    uint64_t m_k1;
};

} // namespace flowmesh2::net

#endif // B3_FLOWMESH2_NET_SHORT_ID_H
