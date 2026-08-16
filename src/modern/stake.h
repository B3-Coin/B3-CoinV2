// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_MODERN_STAKE_H
#define B3COIN_MODERN_STAKE_H

#include <consensus/amount.h>
#include <primitives/transaction.h>
#include <script/script.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace modern {

/**
 * The on-chain carrier of a STAKE Policy Output (v1 wire form — a design
 * PROPOSAL embodied in code; it requires explicit ratification before any
 * mainnet H/X is pinned, and is exercised on regtest only until then).
 *
 * A STAKE output is an ordinary CTxOut whose scriptPubKey is
 *
 *     PUSH(38: "B3S1" || validator_key[32] || reserved[2]=0) OP_DROP
 *     <owner script...>
 *
 * The leading push carries the consensus-visible stake data and is dropped
 * before the REAL locking conditions — the owner script suffix — execute,
 * so the principal stays spendable by (and only by) the owner: unstaking or
 * cancelling is an ordinary spend, and a spent STAKE output simply stops
 * counting (self-policing, exactly like the transition design requires).
 * The validator key is carried as 32 opaque bytes; its cryptographic
 * interpretation belongs to the modern PoS specification and is NOT decided
 * here.
 *
 * Recognition is deterministic on the magic: any output whose first push is
 * exactly STAKE_PAYLOAD_SIZE bytes beginning with "B3S1" claims to be a
 * STAKE output, and a claiming output that violates any v1 constraint
 * (missing OP_DROP, missing owner script, zero validator key, non-zero
 * reserved bytes, zero amount) is INVALID in a modern-era block — never
 * silently reinterpreted as an ordinary output. Outputs whose first push
 * has a different size or magic are ordinary outputs.
 */
inline constexpr std::array<unsigned char, 4> STAKE_MAGIC{'B', '3', 'S', '1'};
inline constexpr size_t STAKE_VALIDATOR_KEY_SIZE{32};
inline constexpr size_t STAKE_RESERVED_SIZE{2};
inline constexpr size_t STAKE_PAYLOAD_SIZE{STAKE_MAGIC.size() + STAKE_VALIDATOR_KEY_SIZE +
                                           STAKE_RESERVED_SIZE};

//! The consensus view of one STAKE output.
struct StakeOutputView {
    CAmount amount{0};
    std::array<unsigned char, STAKE_VALIDATOR_KEY_SIZE> validator_key{};
    CScript owner_script{};
};

//! Build the v1 STAKE scriptPubKey.
inline CScript MakeStakeScript(const std::array<unsigned char, STAKE_VALIDATOR_KEY_SIZE>& validator_key,
                               const CScript& owner_script)
{
    std::vector<unsigned char> payload;
    payload.reserve(STAKE_PAYLOAD_SIZE);
    payload.insert(payload.end(), STAKE_MAGIC.begin(), STAKE_MAGIC.end());
    payload.insert(payload.end(), validator_key.begin(), validator_key.end());
    payload.insert(payload.end(), STAKE_RESERVED_SIZE, 0x00);
    CScript script;
    script << payload << OP_DROP;
    script.insert(script.end(), owner_script.begin(), owner_script.end());
    return script;
}

//! Whether the script's first push claims the STAKE magic (payload size and
//! magic match). A claiming script is either a valid STAKE output or an
//! invalid one — never an ordinary output.
inline bool ClaimsStakeMagic(const CScript& script)
{
    CScript::const_iterator it{script.begin()};
    opcodetype opcode;
    std::vector<unsigned char> data;
    if (!script.GetOp(it, opcode, data)) return false;
    return data.size() == STAKE_PAYLOAD_SIZE &&
           std::equal(STAKE_MAGIC.begin(), STAKE_MAGIC.end(), data.begin());
}

/**
 * Parse a claiming output into its consensus view. Returns std::nullopt
 * (with `error` set) for a claiming output that violates any v1 constraint;
 * must only be called when ClaimsStakeMagic() is true.
 */
inline std::optional<StakeOutputView> ParseStakeOutput(const CTxOut& out, std::string& error)
{
    CScript::const_iterator it{out.scriptPubKey.begin()};
    opcodetype opcode;
    std::vector<unsigned char> data;
    if (!out.scriptPubKey.GetOp(it, opcode, data) || data.size() != STAKE_PAYLOAD_SIZE) {
        error = "stake payload malformed";
        return std::nullopt;
    }
    if (!out.scriptPubKey.GetOp(it, opcode) || opcode != OP_DROP) {
        error = "stake payload not followed by OP_DROP";
        return std::nullopt;
    }
    if (it == out.scriptPubKey.end()) {
        error = "stake output has no owner script";
        return std::nullopt;
    }
    StakeOutputView view;
    view.amount = out.nValue;
    std::copy(data.begin() + STAKE_MAGIC.size(),
              data.begin() + STAKE_MAGIC.size() + STAKE_VALIDATOR_KEY_SIZE,
              view.validator_key.begin());
    view.owner_script = CScript{it, out.scriptPubKey.end()};
    if (view.amount <= 0) {
        error = "stake principal must be positive";
        return std::nullopt;
    }
    bool key_nonzero{false};
    for (const unsigned char b : view.validator_key) key_nonzero |= (b != 0);
    if (!key_nonzero) {
        error = "stake validator key is zero";
        return std::nullopt;
    }
    for (size_t i{STAKE_MAGIC.size() + STAKE_VALIDATOR_KEY_SIZE}; i < STAKE_PAYLOAD_SIZE; ++i) {
        if (data[i] != 0) {
            error = "stake reserved bytes must be zero";
            return std::nullopt;
        }
    }
    return view;
}

/**
 * Modern-era per-transaction STAKE rule: every output claiming the STAKE
 * magic must parse as a valid v1 STAKE output. Returns false with `error`
 * set on the first invalid claiming output.
 */
inline bool CheckStakeOutputs(const CTransaction& tx, std::string& error)
{
    for (const CTxOut& out : tx.vout) {
        if (!ClaimsStakeMagic(out.scriptPubKey)) continue;
        if (!ParseStakeOutput(out, error)) return false;
    }
    return true;
}

} // namespace modern

#endif // B3COIN_MODERN_STAKE_H
