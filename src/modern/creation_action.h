// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_MODERN_CREATION_ACTION_H
#define B3COIN_MODERN_CREATION_ACTION_H

#include <serialize.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace modern {

/**
 * Generic OUTPUT-BOUND creation-action framing — the NEUTRAL layer
 * (owner ruling 2026-08-17). Input proofs authorize spending a previous
 * output; a creation action authorizes CREATING one of the transition's
 * own outputs. This header carries only the generic frame and the
 * bounded collection codec. Historical action types 1 and 2 are permanently
 * reserved/superseded. Their numeric type/version pairs remain registered so
 * they can never acquire new meaning, but their abandoned payload codecs and
 * proof builders are not part of the live tree. The proof-free modern FN PoD
 * declaration is type 6. Semantic layers depend on this header — never the
 * reverse — and modern/proof.h depends only on this neutral layer.
 *
 * Registry of (action_type, action_version). Unknown pairs are INVALID,
 * never ignored — the strict section decoder rejects them outright, and
 * any future consensus validation MUST do the same (the same
 * fail-closed rule modern/policy.h applies to unknown policies). A
 * future action type requires an explicit registry entry and its own
 * versioned review.
 *
 *  - Type 1: the FN claim of the abandoned funding-signature design —
 *    RESERVED/SUPERSEDED (owner ruling 2026-08-17/18, conflict register
 *    C-R4). Its numeric pair is reserved and permanently inactive. Never
 *    reuse or reinterpret it.
 *  - Type 2: the abandoned legacy FN proof carrier. RESERVED/SUPERSEDED;
 *    historical FN units are created deterministically by the H+1 coinbase.
 *  - Type 6: proof-free modern FN PoD creation. Its exact eight-byte payload
 *    binds the priced modern slot and the amount-1 FN output it creates; the
 *    native B3 accounting gap supplies the disintegration.
 */
inline constexpr uint16_t CREATION_ACTION_FN_CLAIM{1}; // RESERVED/SUPERSEDED
inline constexpr uint16_t FN_CLAIM_ACTION_VERSION_V1{1};
inline constexpr uint16_t CREATION_ACTION_LEGACY_FN_ISSUANCE{2};
inline constexpr uint16_t LEGACY_FN_ISSUANCE_ACTION_VERSION_V1{1};
//! Type 3: colored-asset genesis (issuance) — carries the asset's bounded,
//! immutable genesis record (modern/asset.h AssetGenesisV1), whose
//! commitment is bound into the AssetId. Owner ruling 2026-08-22.
inline constexpr uint16_t CREATION_ACTION_ASSET_ISSUANCE{3};
//! RESERVED numbers (owner-frozen 2026-08-23) for the Modern Payload Area
//! record types of the finality gadget: 4 = FINALITY_CERTIFICATE (the BLS
//! certificate payload bound to a FINALITY_CERT cell), 5 =
//! FINALITY_KEY_EVIDENCE (BIP340 binding authorization + BLS proof of
//! possession bound to a FINALITY_KEY cell). NOT registered here: the
//! registry below still rejects them, and when the MPA codec learns to
//! decode them (implementation plan, Commit 5) "known but not activated"
//! remains INVALID until the activation height — recognition never implies
//! activation.
inline constexpr uint16_t CREATION_ACTION_FINALITY_CERTIFICATE{4};
inline constexpr uint16_t CREATION_ACTION_FINALITY_KEY_EVIDENCE{5};
inline constexpr uint16_t ASSET_ISSUANCE_ACTION_VERSION_V1{1};
inline constexpr uint16_t CREATION_ACTION_MODERN_FN_POD{6};
inline constexpr uint16_t MODERN_FN_POD_ACTION_VERSION_V1{1};
//! Type 7 is a FlowMesh FN-seat binding record in the Modern Payload Area.
//! Like finality types 4/5 it is not a standalone CreationAction.
inline constexpr uint16_t CREATION_ACTION_FLOWMESH_SEAT_BINDING{7};
inline constexpr uint16_t FLOWMESH_SEAT_BINDING_ACTION_VERSION_V1{1};
//! Types 8/9 are FlowMesh chain-authorization records in the MPA, not
//! standalone CreationActions: 8 commits a certified checkpoint and 9 proves
//! one checkpointed vault effect. Their numbers are frozen and append-only.
inline constexpr uint16_t CREATION_ACTION_FLOWMESH_CHECKPOINT{8};
inline constexpr uint16_t FLOWMESH_CHECKPOINT_ACTION_VERSION_V1{1};
inline constexpr uint16_t CREATION_ACTION_FLOWMESH_VAULT_PROOF{9};
inline constexpr uint16_t FLOWMESH_VAULT_PROOF_ACTION_VERSION_V1{1};
//! Type 10 is the independently gated Ethereum bridge record. It is an MPA
//! record (never a standalone CreationAction) whose strict inner kind carries
//! light-client bootstrap/update evidence, bounded execution backfill,
//! deposit-mint proofs, or a managed-v1 burn/release request.
inline constexpr uint16_t CREATION_ACTION_BRIDGE{10};
inline constexpr uint16_t BRIDGE_ACTION_VERSION_V1{1};

//! Whether a (type, version) pair is a registered creation action.
//! Registration keeps a pair DECODABLE at the framing layer (so
//! reserved superseded bytes stay well-defined); semantic acceptance is
//! each action's own dispatch.
inline constexpr bool IsKnownCreationAction(const uint16_t action_type,
                                            const uint16_t action_version)
{
    return (action_type == CREATION_ACTION_FN_CLAIM &&
            action_version == FN_CLAIM_ACTION_VERSION_V1) ||
           (action_type == CREATION_ACTION_LEGACY_FN_ISSUANCE &&
            action_version == LEGACY_FN_ISSUANCE_ACTION_VERSION_V1) ||
           (action_type == CREATION_ACTION_ASSET_ISSUANCE &&
            action_version == ASSET_ISSUANCE_ACTION_VERSION_V1) ||
           (action_type == CREATION_ACTION_MODERN_FN_POD &&
            action_version == MODERN_FN_POD_ACTION_VERSION_V1);
}

/**
 * Decode bounds, all enforced BEFORE allocation.
 *
 *  - MAX_CREATION_ACTION_PAYLOAD: one action's payload. 4,000 bytes —
 *    the segregated proof area's existing governing per-payload bound
 *    (modern/proof.h MAX_TRANSITION_PROOF_SIZE; pinned equal there by
 *    static_assert).
 *  - MAX_CREATION_ACTIONS_PER_TRANSITION: 64 — owner-ratified
 *    (2026-08-17), anchored to the existing per-collection cardinality
 *    precedent (MAX_VAULT_RECEIPTS_PER_PROOF = 64).
 *  - MAX_CREATION_ACTION_SECTION_SIZE: 20,000 serialized bytes for the
 *    whole action section — owner-ratified (2026-08-17). After framing
 *    (count byte + 7 bytes per max-size frame) this permits FOUR
 *    maximum-size 4,000-byte actions, or many small ones.
 */
inline constexpr size_t MAX_CREATION_ACTION_PAYLOAD{4000};
inline constexpr size_t MAX_CREATION_ACTIONS_PER_TRANSITION{64};
inline constexpr size_t MAX_CREATION_ACTION_SECTION_SIZE{20'000};

/**
 * The generic frame: type, version, opaque bounded payload. Wire form
 * (standard serialization; the Commit-3 standalone codec bytes are
 * preserved exactly):
 *
 *     uint16 action_type (LE) || uint16 action_version (LE)
 *     || compactSize(payload_len) || payload
 */
struct CreationAction {
    uint16_t action_type{0};
    uint16_t action_version{0};
    std::vector<unsigned char> payload{};

    SERIALIZE_METHODS(CreationAction, obj)
    {
        READWRITE(obj.action_type, obj.action_version, obj.payload);
    }

    friend bool operator==(const CreationAction& a, const CreationAction& b)
    {
        return a.action_type == b.action_type && a.action_version == b.action_version &&
               a.payload == b.payload;
    }
};

namespace detail {

//! Bounded CANONICAL compact-size read from a cursor over `data`.
//! Rejects truncation and non-canonical encodings.
inline bool ReadCompact(std::span<const unsigned char> data, size_t& cursor, uint64_t& out)
{
    if (cursor >= data.size()) return false;
    const uint8_t first{data[cursor++]};
    if (first < 253) {
        out = first;
        return true;
    }
    size_t width{0};
    uint64_t min{0};
    if (first == 253) {
        width = 2;
        min = 253;
    } else if (first == 254) {
        width = 4;
        min = 0x10000;
    } else {
        width = 8;
        min = 0x100000000;
    }
    if (data.size() - cursor < width) return false;
    uint64_t value{0};
    for (size_t i{0}; i < width; ++i) {
        value |= uint64_t{data[cursor + i]} << (8 * i);
    }
    cursor += width;
    if (value < min) return false; // non-canonical
    out = value;
    return true;
}

//! Append a canonical compact size.
inline void WriteCompact(std::vector<unsigned char>& out, uint64_t value)
{
    if (value < 253) {
        out.push_back(static_cast<unsigned char>(value));
    } else if (value <= 0xffff) {
        out.push_back(253);
        out.push_back(value & 0xff);
        out.push_back((value >> 8) & 0xff);
    } else if (value <= 0xffffffff) {
        out.push_back(254);
        for (int i{0}; i < 4; ++i) out.push_back((value >> (8 * i)) & 0xff);
    } else {
        out.push_back(255);
        for (int i{0}; i < 8; ++i) out.push_back((value >> (8 * i)) & 0xff);
    }
}

//! Serialized length of a canonical compact size.
inline constexpr size_t CompactSizeLen(const uint64_t value)
{
    if (value < 253) return 1;
    if (value <= 0xffff) return 3;
    if (value <= 0xffffffff) return 5;
    return 9;
}

//! Little-endian u16 read with bounds (cursor validated BEFORE any
//! subtraction so the arithmetic can never underflow).
inline bool ReadU16(std::span<const unsigned char> data, size_t& cursor, uint16_t& out)
{
    if (cursor > data.size() || data.size() - cursor < 2) return false;
    out = static_cast<uint16_t>(data[cursor]) |
          (static_cast<uint16_t>(data[cursor + 1]) << 8);
    cursor += 2;
    return true;
}

//! Little-endian u32 read with bounds.
inline bool ReadU32(std::span<const unsigned char> data, size_t& cursor, uint32_t& out)
{
    if (cursor > data.size() || data.size() - cursor < 4) return false;
    out = 0;
    for (size_t i{0}; i < 4; ++i) out |= static_cast<uint32_t>(data[cursor + i]) << (8 * i);
    cursor += 4;
    return true;
}

//! Little-endian u64 read with bounds.
inline bool ReadU64(std::span<const unsigned char> data, size_t& cursor, uint64_t& out)
{
    if (cursor > data.size() || data.size() - cursor < 8) return false;
    out = 0;
    for (size_t i{0}; i < 8; ++i) out |= static_cast<uint64_t>(data[cursor + i]) << (8 * i);
    cursor += 8;
    return true;
}

//! Fixed-width raw byte read with bounds; writes into `out[0..n)`.
inline bool ReadBytes(std::span<const unsigned char> data, size_t& cursor, unsigned char* out,
                      const size_t n)
{
    if (cursor > data.size() || data.size() - cursor < n) return false;
    std::copy(data.begin() + cursor, data.begin() + cursor + n, out);
    cursor += n;
    return true;
}

} // namespace detail

//! Serialized size of one action frame.
inline size_t CreationActionSerializedSize(const CreationAction& action)
{
    return 2 + 2 + detail::CompactSizeLen(action.payload.size()) + action.payload.size();
}

/**
 * Encode the action SECTION: compactSize(count) followed by each frame —
 * byte-identical to the standard serialization of
 * std::vector<CreationAction>, so exactly one wire grammar exists.
 * Returns std::nullopt when any bound is violated (count, per-payload,
 * aggregate section size) or an unknown (type, version) appears.
 */
inline std::optional<std::vector<unsigned char>> EncodeCreationActionSection(
    const std::vector<CreationAction>& actions)
{
    if (actions.size() > MAX_CREATION_ACTIONS_PER_TRANSITION) return std::nullopt;
    size_t total{detail::CompactSizeLen(actions.size())};
    std::vector<unsigned char> out;
    detail::WriteCompact(out, actions.size());
    for (const CreationAction& action : actions) {
        if (!IsKnownCreationAction(action.action_type, action.action_version)) {
            return std::nullopt;
        }
        if (action.payload.size() > MAX_CREATION_ACTION_PAYLOAD) return std::nullopt;
        const size_t frame{CreationActionSerializedSize(action)};
        // Checked arithmetic: both operands are bounded well below
        // overflow, but reject explicitly rather than assume.
        if (total > MAX_CREATION_ACTION_SECTION_SIZE - frame) return std::nullopt;
        total += frame;
        out.push_back(action.action_type & 0xff);
        out.push_back((action.action_type >> 8) & 0xff);
        out.push_back(action.action_version & 0xff);
        out.push_back((action.action_version >> 8) & 0xff);
        detail::WriteCompact(out, action.payload.size());
        out.insert(out.end(), action.payload.begin(), action.payload.end());
    }
    return out;
}

/**
 * Strict, allocation-bounded decode of an action section from `data`
 * starting at `cursor` (advanced past the section on success). Enforces:
 * canonical compact sizes; the count bound BEFORE any element is read;
 * known (type, version) pairs only — unknown actions are invalid, never
 * ignored; per-payload and aggregate section bounds with checked
 * arithmetic BEFORE each allocation; and no reliance on trailing state
 * (the caller checks overall exhaustion).
 */
inline bool DecodeCreationActionSection(std::span<const unsigned char> data, size_t& cursor,
                                        std::vector<CreationAction>& out, std::string& error)
{
    uint64_t count{0};
    if (!detail::ReadCompact(data, cursor, count)) {
        error = "truncated or non-canonical action count";
        return false;
    }
    if (count > MAX_CREATION_ACTIONS_PER_TRANSITION) {
        error = "creation-action count exceeds the transition bound";
        return false;
    }
    // A frame is at least 5 bytes; a count the remaining bytes cannot
    // possibly hold is rejected before any element work.
    if (count > (data.size() - cursor) / 5) {
        error = "creation-action count exceeds the available bytes";
        return false;
    }
    size_t total{detail::CompactSizeLen(count)};
    std::vector<CreationAction> actions;
    actions.reserve(static_cast<size_t>(count));
    for (uint64_t i{0}; i < count; ++i) {
        CreationAction action;
        if (!detail::ReadU16(data, cursor, action.action_type) ||
            !detail::ReadU16(data, cursor, action.action_version)) {
            error = "truncated creation-action header";
            return false;
        }
        if (!IsKnownCreationAction(action.action_type, action.action_version)) {
            error = "unknown creation-action type or version";
            return false;
        }
        uint64_t len{0};
        if (!detail::ReadCompact(data, cursor, len)) {
            error = "truncated or non-canonical action payload length";
            return false;
        }
        if (len > MAX_CREATION_ACTION_PAYLOAD) {
            error = "creation-action payload exceeds the proof-area bound";
            return false;
        }
        if (data.size() - cursor < len) {
            error = "truncated creation-action payload";
            return false;
        }
        const size_t frame{2 + 2 + detail::CompactSizeLen(len) + static_cast<size_t>(len)};
        if (total > MAX_CREATION_ACTION_SECTION_SIZE - frame) {
            error = "creation-action section exceeds the aggregate bound";
            return false;
        }
        total += frame;
        action.payload.assign(data.begin() + cursor, data.begin() + cursor + len);
        cursor += len;
        actions.push_back(std::move(action));
    }
    out = std::move(actions);
    return true;
}

} // namespace modern

#endif // B3COIN_MODERN_CREATION_ACTION_H
