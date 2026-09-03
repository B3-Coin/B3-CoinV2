// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_MODERN_FN_POD_H
#define B3COIN_MODERN_FN_POD_H

#include <consensus/amount.h>
#include <consensus/fn_params.h>
#include <consensus/params.h>
#include <modern/creation_action.h>
#include <primitives/transaction.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace modern {

/**
 * Proof-free modern FN PoD declaration (MPA type 6, version 1).
 *
 * The payload is exactly two uint32 values in network byte order: the number
 * of modern FN units created before this transaction, followed by the vout
 * index of the amount-1 FN owner output it creates. Binding the slot makes the
 * tier price explicit and prevents a mempool fee from changing meaning after
 * another creation confirms. The historical
 * 4,000-byte proof formats (types 1 and 2) are unrelated and remain dead.
 * Authorization comes from the native input/output accounting gap and the
 * branch-local lifetime counter, both checked by consensus.
 */
struct ModernFnPodActionV1 {
    uint32_t created_before{0};
    uint32_t output_index{0};

    friend bool operator==(const ModernFnPodActionV1&, const ModernFnPodActionV1&) = default;
};

inline constexpr size_t MODERN_FN_POD_ACTION_V1_SIZE{8};

inline std::vector<unsigned char> EncodeModernFnPodPayload(const ModernFnPodActionV1& action)
{
    return {
        static_cast<unsigned char>((action.created_before >> 24) & 0xff),
        static_cast<unsigned char>((action.created_before >> 16) & 0xff),
        static_cast<unsigned char>((action.created_before >> 8) & 0xff),
        static_cast<unsigned char>(action.created_before & 0xff),
        static_cast<unsigned char>((action.output_index >> 24) & 0xff),
        static_cast<unsigned char>((action.output_index >> 16) & 0xff),
        static_cast<unsigned char>((action.output_index >> 8) & 0xff),
        static_cast<unsigned char>(action.output_index & 0xff),
    };
}

inline bool DecodeModernFnPodPayload(const uint16_t type,
                                     const uint16_t version,
                                     const std::vector<unsigned char>& payload,
                                     ModernFnPodActionV1& out,
                                     std::string& error)
{
    if (type != CREATION_ACTION_MODERN_FN_POD ||
        version != MODERN_FN_POD_ACTION_VERSION_V1) {
        error = "not a modern FN PoD declaration";
        return false;
    }
    if (payload.size() != MODERN_FN_POD_ACTION_V1_SIZE) {
        error = "modern FN PoD declaration has the wrong size";
        return false;
    }
    out.created_before = (uint32_t{payload[0]} << 24) |
                         (uint32_t{payload[1]} << 16) |
                         (uint32_t{payload[2]} << 8) |
                         uint32_t{payload[3]};
    out.output_index = (uint32_t{payload[4]} << 24) |
                       (uint32_t{payload[5]} << 16) |
                       (uint32_t{payload[6]} << 8) |
                       uint32_t{payload[7]};
    error.clear();
    return true;
}

inline CreationAction MakeModernFnPodAction(const uint32_t created_before,
                                            const uint32_t output_index)
{
    return CreationAction{CREATION_ACTION_MODERN_FN_POD,
                          MODERN_FN_POD_ACTION_VERSION_V1,
                          EncodeModernFnPodPayload(
                              ModernFnPodActionV1{created_before, output_index})};
}

inline CMpaRecord MakeModernFnPodRecord(const uint32_t created_before,
                                        const uint32_t output_index)
{
    const CreationAction action{MakeModernFnPodAction(created_before, output_index)};
    CMpaRecord record;
    record.payload_type = action.action_type;
    record.payload_version = action.action_version;
    record.payload = action.payload;
    return record;
}

inline bool DecodeModernFnPodAction(const CreationAction& action,
                                    ModernFnPodActionV1& out,
                                    std::string& error)
{
    return DecodeModernFnPodPayload(action.action_type, action.action_version,
                                    action.payload, out, error);
}

inline bool DecodeModernFnPodRecord(const CMpaRecord& record,
                                    ModernFnPodActionV1& out,
                                    std::string& error)
{
    return DecodeModernFnPodPayload(record.payload_type, record.payload_version,
                                    record.payload, out, error);
}

inline size_t CountModernFnPodDeclarations(const CTransaction& tx)
{
    size_t count{0};
    for (const CMpaRecord& record : tx.mpa) {
        if (record.payload_type == CREATION_ACTION_MODERN_FN_POD) ++count;
    }
    return count;
}

inline bool HasModernFnPodDeclaration(const CTransaction& tx)
{
    return CountModernFnPodDeclarations(tx) != 0;
}

//! Number of proof-free modern creations available after the fixed historical
//! genesis. Extinguished FN units never increase this capacity.
inline std::optional<uint32_t> ModernFnCapacity(const Consensus::Params& params)
{
    if (params.fn_genesis_manifest.size() > Consensus::MAX_FN_EVER_ISSUED) {
        return std::nullopt;
    }
    return Consensus::MAX_FN_EVER_ISSUED -
           static_cast<uint32_t>(params.fn_genesis_manifest.size());
}

//! Required native-B3 accounting-gap destruction for the next modern unit.
//! `created_before` excludes the historical genesis count.
inline CAmount RequiredFnPodDisintegration(const uint32_t created_before)
{
    if (created_before < 500) return CAmount{15'000} * KILO_COIN;
    if (created_before < 1'000) return CAmount{30'000} * KILO_COIN;
    return CAmount{60'000} * KILO_COIN;
}

static_assert(CAmount{60'000} * KILO_COIN > 0);
static_assert(CAmount{60'000} * KILO_COIN <= MAX_MONEY);

} // namespace modern

#endif // B3COIN_MODERN_FN_POD_H
