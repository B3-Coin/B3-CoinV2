// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_CORE_OUTCOME_H
#define B3_FLOWMESH2_CORE_OUTCOME_H

#include <flowmesh2/core/action.h>
#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/codes.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <ios>
#include <span>
#include <string_view>
#include <vector>

/** Certified execution outcomes (slice R4; ARCHITECTURE.md sections 3, 11).
 *
 * The engine (R7) emits one OutcomeV2 per consumed fact, one per executed
 * action (including deterministic rejections, DUR-5) and one
 * FEES_DISTRIBUTED outcome per quote asset. Their leaves, in emission order,
 * form the header's outcomes_root, so an outcome is certified data that a
 * client verifies with b3verify (R11): the code, the fills (VerifyFill
 * recomputes quote and fee) and the balance deltas (VerifyBalanceTransition).
 *
 * Encoding (Bitcoin serialization, little-endian):
 *   u8 source | subject | account | u64 nonce | u16 code | u64 remaining_lots
 *   | CompactSize #fills, each Fill (137 B) | CompactSize #deltas, each
 *   BalanceDelta (80 B)
 * Fill: maker | taker | market | u8 taker_side | u64 price_ticks | u64 lots
 *   | i64 quote_atoms | i64 seller_fee | i64 seller_quote_before
 * BalanceDelta: account | asset | i64 available_delta | i64 reserved_delta
 *
 * Meaning (PROVISIONAL until R6/R7 pin the engine; D4, D5):
 * - source FACT: subject = FactId, account = the credited account, nonce 0.
 *   source ACTION: subject = ActionId, account = the signer, nonce = its
 *   nonce. source SLOT: subject = the quote AssetId of a FEES_DISTRIBUTED
 *   outcome, account zero, nonce 0.
 * - fills: the fills this action took part in as taker, in execution order.
 *   quote_atoms = QuoteAtoms(market, lots, price_ticks); seller_fee is the
 *   seller's increment (core/fee_math.h) given seller_quote_before, the
 *   seller order's cumulative matched quote before this fill.
 * - deltas: every balance this outcome changed, one entry per (account,
 *   asset), sorted by (account, asset), no all-zero entry. A delta is in
 *   [-MAX_ATOMS, MAX_ATOMS].
 * - remaining_lots: lots left resting (PLACE), else 0.
 *
 * Decode(bytes, profile) is exact: more than MaxFills(profile) fills or
 * MaxDeltas(profile) deltas, or input above MaxEncodedSize(profile), is
 * OVERSIZE (counts checked before allocation); an unknown source, code or
 * side, an amount outside [0, MAX_ATOMS] or a delta outside
 * [-MAX_ATOMS, MAX_ATOMS] is DECODE. The sort and aggregation rules are the
 * engine's output contract (R7) and the verifier's check (R11), not a codec
 * rule. Encode() is total so that LeafHash() exists for any value.
 *
 * LeafHash() = Tagged(OUTCOME, Encode()): the Merkle leaf value.
 */
namespace flowmesh2 {

//! Who an outcome belongs to. APPEND-ONLY registry; zero is never a value.
enum class OutcomeSource : uint8_t {
    FACT = 1,
    ACTION = 2,
    SLOT = 3,
};

bool IsKnown(OutcomeSource source);
std::string_view ToString(OutcomeSource source);

struct Fill {
    OrderId maker;
    OrderId taker;
    MarketId market;
    Side taker_side{Side::BUY};
    uint64_t price_ticks{0};
    uint64_t lots{0};
    Atoms quote_atoms{0};
    Atoms seller_fee{0};
    Atoms seller_quote_before{0};

    static constexpr size_t ENCODED_BYTES{32 + 32 + 32 + 1 + 8 + 8 + 8 + 8 + 8};
    friend bool operator==(const Fill&, const Fill&) = default;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << maker << taker << market << static_cast<uint8_t>(taker_side) << price_ticks << lots << quote_atoms
          << seller_fee << seller_quote_before;
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        uint8_t side_byte{0};
        s >> maker >> taker >> market >> side_byte >> price_ticks >> lots >> quote_atoms >> seller_fee >>
            seller_quote_before;
        taker_side = detail::ReadEnumByte<Side>(side_byte, "unknown Fill side");
        detail::CheckAtomsOnWire(quote_atoms);
        detail::CheckAtomsOnWire(seller_fee);
        detail::CheckAtomsOnWire(seller_quote_before);
    }
};

struct BalanceDelta {
    AccountId account;
    AssetId asset;
    int64_t available_delta{0};
    int64_t reserved_delta{0};

    static constexpr size_t ENCODED_BYTES{32 + 32 + 8 + 8};
    friend bool operator==(const BalanceDelta&, const BalanceDelta&) = default;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << account << asset << available_delta << reserved_delta;
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> account >> asset >> available_delta >> reserved_delta;
        if (available_delta < -MAX_ATOMS || available_delta > MAX_ATOMS || reserved_delta < -MAX_ATOMS ||
            reserved_delta > MAX_ATOMS) {
            throw std::ios_base::failure("BalanceDelta outside [-MAX_ATOMS, MAX_ATOMS]");
        }
    }
};

struct OutcomeV2 {
    OutcomeSource source{OutcomeSource::ACTION};
    uint256 subject;
    AccountId account;
    uint64_t nonce{0};
    OutcomeCode code{OutcomeCode::RESTING};
    uint64_t remaining_lots{0};
    std::vector<Fill> fills;
    std::vector<BalanceDelta> deltas;

    friend bool operator==(const OutcomeV2&, const OutcomeV2&) = default;

    //! Most fills in one outcome: profile.max_fills_per_order.
    static size_t MaxFills(const ProfileParams& profile);
    //! Most deltas in one outcome: 2 * (max_fills_per_order + 1), the taker's
    //! two assets plus two per maker. PROVISIONAL until R7 pins the engine.
    static size_t MaxDeltas(const ProfileParams& profile);
    static size_t MaxEncodedSize(const ProfileParams& profile);

    std::vector<unsigned char> Encode() const;
    static Result<OutcomeV2> Decode(std::span<const unsigned char> bytes, const ProfileParams& profile);
    //! Tagged(OUTCOME, Encode()).
    uint256 LeafHash() const;
};

} // namespace flowmesh2

#endif // B3_FLOWMESH2_CORE_OUTCOME_H
