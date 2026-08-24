// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3COIN_BRIDGE_DEPOSIT_H
#define B3COIN_BRIDGE_DEPOSIT_H

#include <bridge/rlp.h>
#include <crypto/keccak256.h>
#include <uint256.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

/** Ethereum receipt decoding and B3DepositVault event extraction — the last
 *  pure-verification piece of the ETH -> B3 deposit leg (header-only, not
 *  reachable from consensus; stage 2-3 of the bridge staged build order).
 *
 *  Trust chain: eth_light_client.h proves the finalized receipts_root;
 *  mpt.h proves a receipt against it; this file decodes that receipt and
 *  extracts Deposit events from the vault contract
 *  (contracts/B3DepositVault.sol):
 *
 *    event Deposit(uint64 indexed depositId, address indexed token,
 *                  uint256 amount, bytes32 b3Recipient);
 *
 *  Everything is strict: canonical RLP integers only, exact topic/data
 *  shapes, exact padding. What a mint of the proven deposit MEANS on B3
 *  (asset policy, recipient semantics) is consensus stage 4 and stays
 *  gated on the owner's A3 rulings.
 */
namespace bridge {

using EthAddress = std::array<unsigned char, 20>;

struct EthLog {
    EthAddress address{};
    std::vector<uint256> topics{};
    std::vector<unsigned char> data{};
};

struct EthReceipt {
    uint8_t type{0};       // 0 legacy; 1..4 typed (EIP-2718 envelope)
    bool status{false};    // post-Byzantium status byte
    uint64_t cumulative_gas{0};
    std::vector<EthLog> logs{};
};

namespace deposit_detail {

//! Strict canonical big-endian uint from an RLP string payload.
inline std::optional<uint64_t> RlpUint64(const RlpItem& item)
{
    if (item.is_list || item.payload.size() > 8) return std::nullopt;
    if (!item.payload.empty() && item.payload[0] == 0x00) return std::nullopt;
    uint64_t v{0};
    for (unsigned char b : item.payload) v = (v << 8) | b;
    return v;
}

} // namespace deposit_detail

/** Decode a receipts-trie VALUE (as returned by VerifyMptProof) into a
 *  receipt. Handles the legacy shape and EIP-2718 typed envelopes 1..4.
 *  Pre-Byzantium state-root receipts are rejected — the light client only
 *  ever proves recent finalized blocks. */
inline std::optional<EthReceipt> DecodeReceipt(std::span<const unsigned char> value)
{
    using deposit_detail::RlpUint64;
    if (value.empty()) return std::nullopt;
    EthReceipt out;
    if (value[0] <= 0x04) { // typed envelope (0x00 is not a valid first RLP byte here)
        if (value[0] == 0x00 || value.size() < 2) return std::nullopt;
        out.type = value[0];
        value = value.subspan(1);
    }
    const auto top{RlpDecode(value)};
    if (!top || !top->is_list) return std::nullopt;
    const auto fields{RlpChildren(*top)};
    if (!fields || fields->size() != 4) return std::nullopt;

    // status: canonical 0x01 (payload {0x01}) or 0x00 (empty payload).
    const RlpItem& st{(*fields)[0]};
    if (st.is_list || st.payload.size() > 1) return std::nullopt; // state-root form rejected
    out.status = !st.payload.empty();
    if (out.status && st.payload[0] != 0x01) return std::nullopt;

    const auto gas{RlpUint64((*fields)[1])};
    if (!gas) return std::nullopt;
    out.cumulative_gas = *gas;

    const RlpItem& bloom{(*fields)[2]};
    if (bloom.is_list || bloom.payload.size() != 256) return std::nullopt;

    const auto logs{RlpChildren((*fields)[3])};
    if (!logs) return std::nullopt;
    for (const RlpItem& l : *logs) {
        const auto parts{RlpChildren(l)};
        if (!parts || parts->size() != 3) return std::nullopt;
        EthLog log;
        const RlpItem& addr{(*parts)[0]};
        if (addr.is_list || addr.payload.size() != 20) return std::nullopt;
        std::copy(addr.payload.begin(), addr.payload.end(), log.address.begin());
        const auto topics{RlpChildren((*parts)[1])};
        if (!topics || topics->size() > 4) return std::nullopt;
        for (const RlpItem& t : *topics) {
            if (t.is_list || t.payload.size() != 32) return std::nullopt;
            log.topics.emplace_back(std::span<const unsigned char>{t.payload});
        }
        const RlpItem& data{(*parts)[2]};
        if (data.is_list) return std::nullopt;
        log.data.assign(data.payload.begin(), data.payload.end());
        out.logs.push_back(std::move(log));
    }
    return out;
}

// ---------------------------------------------------------------------------
// B3DepositVault event extraction.

struct DepositEvent {
    uint64_t deposit_id{0};
    EthAddress token{};                       // 0x00..00 = native ETH
    std::array<unsigned char, 32> amount{};   // uint256, big-endian
    std::array<unsigned char, 32> b3_recipient{};
};

//! keccak("Deposit(uint64,address,uint256,bytes32)").
inline const uint256& DepositTopic()
{
    static const uint256 topic{[] {
        static constexpr char sig[]{"Deposit(uint64,address,uint256,bytes32)"};
        uint256 out;
        Keccak256().Write({reinterpret_cast<const unsigned char*>(sig), sizeof(sig) - 1}).Finalize(out);
        return out;
    }()};
    return topic;
}

/** Extract every well-formed vault Deposit event from a decoded receipt.
 *  Only logs emitted by `vault` count; the receipt must be a success.
 *  Malformed pseudo-deposits (bad padding, wrong shapes) are skipped, not
 *  errors — other contracts may emit colliding topics. */
inline std::vector<DepositEvent> ExtractDeposits(const EthReceipt& receipt, const EthAddress& vault)
{
    std::vector<DepositEvent> out;
    if (!receipt.status) return out;
    for (const EthLog& log : receipt.logs) {
        if (log.address != vault) continue;
        if (log.topics.size() != 3 || log.topics[0] != DepositTopic()) continue;
        if (log.data.size() != 64) continue;
        DepositEvent ev;
        // topics[1] = uint64 depositId, left-padded to 32 bytes.
        const unsigned char* t1{log.topics[1].begin()};
        bool pad_ok{true};
        for (int i = 0; i < 24; ++i) pad_ok &= (t1[i] == 0);
        if (!pad_ok) continue;
        for (int i = 24; i < 32; ++i) ev.deposit_id = (ev.deposit_id << 8) | t1[i];
        // topics[2] = address token, left-padded to 32 bytes.
        const unsigned char* t2{log.topics[2].begin()};
        for (int i = 0; i < 12; ++i) pad_ok &= (t2[i] == 0);
        if (!pad_ok) continue;
        std::copy(t2 + 12, t2 + 32, ev.token.begin());
        std::copy(log.data.begin(), log.data.begin() + 32, ev.amount.begin());
        std::copy(log.data.begin() + 32, log.data.end(), ev.b3_recipient.begin());
        out.push_back(ev);
    }
    return out;
}

} // namespace bridge

#endif // B3COIN_BRIDGE_DEPOSIT_H
