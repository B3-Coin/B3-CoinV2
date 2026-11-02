// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! The Ethereum bridge's byte-level decoders, which a node runs on anyone's
//! bridge transactions: strict RLP, Merkle-Patricia inclusion proofs,
//! receipts with their vault Deposit events, the execution-header chain a
//! mint walks down to its deposit, and the deposit's recipient and amount.
//! Each target checks that any bytes never crash a decoder and decode only
//! from their canonical encoding. Random bytes rarely get past a hash or
//! shape check, so each target also builds valid values: tries, proofs,
//! receipts and header chains.

#include <arith_uint256.h>
#include <bridge/admission.h>
#include <bridge/deposit.h>
#include <bridge/exec_chain.h>
#include <bridge/mpt.h>
#include <bridge/rlp.h>
#include <consensus/amount.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <test/util/bridge_eth_trie.h>
#include <uint256.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace {

using Bytes = std::vector<unsigned char>;

//! Lists nested deeper than this are decoded but not re-encoded.
constexpr int MAX_REENCODE_DEPTH{64};

//! `item` rebuilt from its parts with the bridge's own encoders. On the way,
//! each child's span as RlpEncodedSpan recovers it (the MPT walker hashes
//! inline nodes by it) must be exactly the bytes the child was decoded from.
//! nullopt when a nested child does not decode (RlpDecode checks only the
//! outer item) or the nesting is deeper than checked.
// NOLINTNEXTLINE(misc-no-recursion)
std::optional<Bytes> Reencode(const bridge::RlpItem& item, const int depth)
{
    if (!item.is_list) return bridge::RlpEncodeBytes(item.payload);
    if (depth >= MAX_REENCODE_DEPTH) return std::nullopt;
    const auto children{bridge::RlpChildren(item)};
    if (!children) return std::nullopt;
    std::vector<Bytes> encoded;
    encoded.reserve(children->size());
    size_t offset{0};
    for (const bridge::RlpItem& child : *children) {
        const std::span<const unsigned char> span{bridge::RlpEncodedSpan(child, item.payload)};
        assert(span.data() == item.payload.data() + offset);
        offset += span.size();
        auto child_encoding{Reencode(child, depth + 1)};
        if (!child_encoding) return std::nullopt;
        assert(std::ranges::equal(*child_encoding, span));
        encoded.push_back(std::move(*child_encoding));
    }
    assert(offset == item.payload.size());
    return bridge::RlpEncodeList(encoded);
}

//! A random item, encoded with the bridge's encoders. Strings cross the
//! short form's 55-byte bound and the one-byte length's 255.
// NOLINTNEXTLINE(misc-no-recursion)
Bytes RandomRlp(FuzzedDataProvider& fdp, const int depth)
{
    if (depth < 4 && fdp.ConsumeBool()) {
        std::vector<Bytes> children;
        LIMITED_WHILE(fdp.ConsumeBool(), 6) children.push_back(RandomRlp(fdp, depth + 1));
        return bridge::RlpEncodeList(children);
    }
    return bridge::RlpEncodeBytes(ConsumeRandomLengthByteVector<unsigned char>(fdp, 300));
}

//! The item `bytes` encodes, under a header the strict decoder refuses: a
//! byte below 0x80 given a string header, the long form where the short one
//! fits, or a length with a leading zero byte.
Bytes NonCanonical(const Bytes& bytes)
{
    const unsigned char first{bytes[0]};
    if (first <= 0x7f) return {0x81, first};
    const unsigned char base{static_cast<unsigned char>(first >= 0xc0 ? 0xc0 : 0x80)};
    Bytes out;
    if (first - base <= 55) {
        out = {static_cast<unsigned char>(base + 56), static_cast<unsigned char>(first - base)};
    } else {
        out = {static_cast<unsigned char>(first + 1), 0x00};
    }
    out.insert(out.end(), bytes.begin() + 1, bytes.end());
    return out;
}

//! A log as Ethereum encodes it inside a receipt.
Bytes EncodeLog(const bridge::EthLog& log)
{
    std::vector<Bytes> topics;
    topics.reserve(log.topics.size());
    for (const uint256& topic : log.topics) {
        topics.push_back(bridge::RlpEncodeBytes(std::span<const unsigned char>{topic.begin(), topic.size()}));
    }
    return bridge::RlpEncodeList({bridge::RlpEncodeBytes(log.address), bridge::RlpEncodeList(topics),
                                  bridge::RlpEncodeBytes(log.data)});
}

//! A receipt's four fields as Ethereum writes them. The decoder checks the
//! bloom's size but keeps no copy, so the caller supplies it.
std::vector<Bytes> ReceiptFields(const bridge::EthReceipt& receipt, const std::span<const unsigned char> bloom)
{
    std::vector<Bytes> logs;
    logs.reserve(receipt.logs.size());
    for (const bridge::EthLog& log : receipt.logs) logs.push_back(EncodeLog(log));
    return {bridge::RlpEncodeUint64(receipt.status ? 1 : 0), bridge::RlpEncodeUint64(receipt.cumulative_gas),
            bridge::RlpEncodeBytes(bloom), bridge::RlpEncodeList(logs)};
}

//! A receipts-trie value as Ethereum writes it: a typed receipt's type
//! byte, then the list of its fields.
Bytes EncodeReceipt(const uint8_t type, const std::vector<Bytes>& fields)
{
    Bytes out;
    if (type != 0) out.push_back(type);
    const Bytes body{bridge::RlpEncodeList(fields)};
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

//! The Deposit log the vault emits for `event`.
bridge::EthLog EncodeDepositLog(const bridge::DepositEvent& event, const bridge::EthAddress& vault)
{
    bridge::EthLog log;
    log.address = vault;
    uint256 id;
    uint256 token;
    for (int i{0}; i < 8; ++i) id.data()[31 - i] = static_cast<unsigned char>(event.deposit_id >> (8 * i));
    std::copy(event.token.begin(), event.token.end(), token.begin() + 12);
    log.topics = {bridge::DepositTopic(), id, token};
    log.data.assign(event.amount.begin(), event.amount.end());
    log.data.insert(log.data.end(), event.b3_recipient.begin(), event.b3_recipient.end());
    return log;
}

bool SameLog(const bridge::EthLog& a, const bridge::EthLog& b)
{
    return a.address == b.address && a.topics == b.topics && a.data == b.data;
}

bool SameDeposit(const bridge::DepositEvent& a, const bridge::DepositEvent& b)
{
    return a.deposit_id == b.deposit_id && a.token == b.token && a.amount == b.amount &&
           a.b3_recipient == b.b3_recipient;
}

template <size_t N>
std::array<unsigned char, N> ConsumeArray(FuzzedDataProvider& fdp)
{
    std::array<unsigned char, N> out{};
    const Bytes bytes{fdp.ConsumeBytes<unsigned char>(N)};
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return out;
}

//! Every decoded vault Deposit is exactly what its log encodes: no other
//! log decodes as one.
void CheckDeposits(const bridge::EthReceipt& receipt, const bridge::EthAddress& vault)
{
    std::vector<bridge::DepositEvent> expected;
    for (size_t i{0}; i < receipt.logs.size(); ++i) {
        const auto event{bridge::ExtractDepositAt(receipt, vault, i)};
        if (!event) continue;
        assert(receipt.status);
        assert(SameLog(EncodeDepositLog(*event, vault), receipt.logs[i]));
        expected.push_back(*event);
    }
    assert(!bridge::ExtractDepositAt(receipt, vault, receipt.logs.size()));
    const auto all{bridge::ExtractDeposits(receipt, vault)};
    assert(std::ranges::equal(all, expected, SameDeposit));
}

//! One header of a chain BuildChain makes, as Ethereum writes it: an RLP
//! list of at least 16 fields whose [0] is the parent hash, [3] the state
//! root, [5] the receipts root and [8] the number.
struct ChainHeader {
    Bytes rlp;
    bridge::ExecAncestor ancestor{}; //!< what the walk returns for it
    bool parses{true};               //!< false: a field the walk refuses
};

bool SameAncestor(const bridge::ExecAncestor& a, const bridge::ExecAncestor& b)
{
    return a.block_number == b.block_number && a.block_hash == b.block_hash &&
           a.receipts_root == b.receipts_root && a.state_root == b.state_root;
}

Bytes HashBytes(const uint256& hash) { return Bytes(hash.begin(), hash.end()); }

//! Headers numbered `numbers`, newest first, each naming the keccak of the
//! next as its parent. Now and then a header has a field the walk refuses,
//! or a state root nothing can be proven against.
std::vector<ChainHeader> BuildChain(FuzzedDataProvider& fdp, const std::vector<uint64_t>& numbers)
{
    std::vector<ChainHeader> chain(numbers.size());
    uint256 parent{ConsumeUInt256(fdp)};
    for (size_t i{numbers.size()}; i-- > 0;) {
        ChainHeader& header{chain[i]};
        header.ancestor.block_number = numbers[i];
        header.ancestor.receipts_root = ConsumeUInt256(fdp);
        header.ancestor.state_root = ConsumeUInt256(fdp);
        const size_t count{fdp.ConsumeIntegralInRange<size_t>(16, 18)};
        std::vector<Bytes> fields;
        fields.reserve(count);
        for (size_t f{0}; f < count; ++f) {
            fields.push_back(bridge::RlpEncodeBytes(ConsumeRandomLengthByteVector<unsigned char>(fdp, 8)));
        }
        fields[0] = bridge::RlpEncodeBytes(HashBytes(parent));
        fields[3] = bridge::RlpEncodeBytes(HashBytes(header.ancestor.state_root));
        fields[5] = bridge::RlpEncodeBytes(HashBytes(header.ancestor.receipts_root));
        fields[8] = bridge::RlpEncodeUint64(numbers[i]);
        switch (fdp.ConsumeIntegralInRange<int>(0, 15)) {
        case 0: { // a state root of another size: the header links, but proves no state
            Bytes state{ConsumeRandomLengthByteVector<unsigned char>(fdp, 40)};
            if (state.size() == 32) state.push_back(0);
            fields[3] = bridge::RlpEncodeBytes(state);
            header.ancestor.state_root.SetNull();
            break;
        }
        case 1: // fewer fields than a post-London header
            fields.resize(15);
            header.parses = false;
            break;
        case 2: { // the number with a leading zero byte
            Bytes number{0x00};
            for (int shift{56}; shift >= 0; shift -= 8) {
                if ((numbers[i] >> shift) != 0) number.push_back(static_cast<unsigned char>(numbers[i] >> shift));
            }
            fields[8] = bridge::RlpEncodeBytes(number);
            header.parses = false;
            break;
        }
        case 3: // a receipts root one byte short
            fields[5] = bridge::RlpEncodeBytes(Bytes(31, 0x11));
            header.parses = false;
            break;
        default:
            break;
        }
        header.rlp = bridge::RlpEncodeList(fields);
        header.ancestor.block_hash = bridge_test::EthTrie::HashOf(header.rlp);
        parent = header.ancestor.block_hash;
    }
    return chain;
}

//! The big-endian 256-bit integer `bytes` hold.
arith_uint256 FromBigEndian(const std::array<unsigned char, 32>& bytes)
{
    arith_uint256 value;
    for (const unsigned char byte : bytes) {
        value <<= 8;
        value |= uint64_t{byte};
    }
    return value;
}

std::array<unsigned char, 32> ToBigEndian(const arith_uint256& value)
{
    const uint256 little{ArithToUint256(value)};
    std::array<unsigned char, 32> out{};
    std::reverse_copy(little.begin(), little.end(), out.begin());
    return out;
}

arith_uint256 PowerOfTen(const unsigned exponent)
{
    arith_uint256 value{1};
    for (unsigned i{0}; i < exponent; ++i) value *= uint32_t{10};
    return value;
}

//! ConvertRawUnitsExact's rule, on 256-bit integers: scale down exactly or
//! not at all, keep within (0, MAX_MONEY], then scale up within it.
std::optional<CAmount> ReferenceConvert(const std::array<unsigned char, 32>& amount, const uint8_t origin,
                                        const uint8_t asset)
{
    if (origin > 18 || asset > 18) return std::nullopt;
    arith_uint256 value{FromBigEndian(amount)};
    const arith_uint256 max_money{static_cast<uint64_t>(MAX_MONEY)};
    if (origin > asset) {
        const arith_uint256 scale{PowerOfTen(static_cast<unsigned>(origin - asset))};
        const arith_uint256 quotient{value / scale};
        if (quotient * scale != value) return std::nullopt;
        value = quotient;
    }
    if (value == 0 || value > max_money) return std::nullopt;
    if (asset > origin) value *= PowerOfTen(static_cast<unsigned>(asset - origin));
    if (value > max_money) return std::nullopt;
    return static_cast<CAmount>(value.GetLow64());
}

} // namespace

FUZZ_TARGET(bridge_rlp)
{
    FuzzedDataProvider fdp{buffer.data(), buffer.size()};
    if (fdp.ConsumeBool()) {
        // Any bytes: never crash, and decode only from their canonical encoding.
        const Bytes bytes{fdp.ConsumeRemainingBytes<unsigned char>()};
        const auto item{bridge::RlpDecode(bytes)};
        if (!item) return;
        assert(item->is_list || !bridge::RlpChildren(*item));
        if (const auto encoding{Reencode(*item, 0)}) assert(*encoding == bytes);
        return;
    }

    // An item built with the encoders decodes, and re-encodes to itself.
    const Bytes bytes{RandomRlp(fdp, 0)};
    const auto item{bridge::RlpDecode(bytes)};
    assert(item);
    const auto encoding{Reencode(*item, 0)};
    assert(encoding && *encoding == bytes);

    // Under a header of any other form it never decodes, alone or in a list.
    const Bytes other{NonCanonical(bytes)};
    assert(!bridge::RlpDecode(other));
    const Bytes wrapped{bridge::RlpEncodeList({other})};
    const auto list{bridge::RlpDecode(wrapped)};
    assert(list && !bridge::RlpChildren(*list));

    // Canonical integers round-trip through the receipt decoder's reader.
    const uint64_t value{fdp.ConsumeIntegral<uint64_t>()};
    const Bytes value_encoding{bridge::RlpEncodeUint64(value)};
    const auto integer{bridge::RlpDecode(value_encoding)};
    assert(integer && bridge::deposit_detail::RlpUint64(*integer) == value);
}

FUZZ_TARGET(bridge_mpt_proof)
{
    FuzzedDataProvider fdp{buffer.data(), buffer.size()};
    if (fdp.ConsumeBool()) {
        // Any key and proof nodes, with the root taken from the first node
        // so that the walk gets past its first check: never crash.
        const Bytes key{ConsumeRandomLengthByteVector<unsigned char>(fdp, 40)};
        std::vector<Bytes> proof;
        LIMITED_WHILE(fdp.remaining_bytes() > 0, 12) {
            proof.push_back(ConsumeRandomLengthByteVector<unsigned char>(fdp, 700));
        }
        const uint256 root{proof.empty() ? uint256{} : bridge_test::EthTrie::HashOf(proof[0])};
        (void)bridge::VerifyMptProof(root, key, proof);
        return;
    }

    // A trie as Ethereum builds it, from short keys (so some are prefixes of
    // others and branch nodes hold values) and short values (so nodes are
    // both inlined and hashed). The builder walks keys independently of the
    // verifier.
    bridge_test::EthTrie trie;
    std::map<Bytes, Bytes> entries;
    LIMITED_WHILE(fdp.ConsumeBool(), 24) {
        const Bytes key{ConsumeRandomLengthByteVector<unsigned char>(fdp, 6)};
        Bytes value{ConsumeRandomLengthByteVector<unsigned char>(fdp, 48)};
        if (value.empty()) value.push_back(0); // a trie stores no empty value
        trie.Insert(key, value);
        entries[key] = value;
    }
    const uint256 root{trie.Root()};

    // Each key's own proof yields its value.
    for (const auto& [key, value] : entries) assert(bridge::VerifyMptProof(root, key, trie.Proof(key)) == value);

    if (!entries.empty()) {
        // An honest proof with one byte changed, its last node missing, or
        // a node more, is refused.
        const Bytes& key{std::next(entries.begin(), fdp.ConsumeIntegralInRange<size_t>(0, entries.size() - 1))->first};
        const std::vector<Bytes> honest{trie.Proof(key)};
        std::vector<Bytes> changed{honest};
        Bytes& node{changed[fdp.ConsumeIntegralInRange<size_t>(0, changed.size() - 1)]};
        node[fdp.ConsumeIntegralInRange<size_t>(0, node.size() - 1)] ^= fdp.ConsumeIntegralInRange<uint8_t>(1, 255);
        assert(!bridge::VerifyMptProof(root, key, changed));
        const std::vector<Bytes> shorter(honest.begin(), std::prev(honest.end()));
        assert(!bridge::VerifyMptProof(root, key, shorter));
        std::vector<Bytes> longer{honest};
        longer.push_back(fdp.ConsumeBool() ? honest[fdp.ConsumeIntegralInRange<size_t>(0, honest.size() - 1)]
                                           : ConsumeRandomLengthByteVector<unsigned char>(fdp, 64));
        assert(!bridge::VerifyMptProof(root, key, longer));
    }

    // Soundness: whatever key and proof are offered (another key's proof,
    // nodes dropped, repeated or swapped), a value is only ever returned for
    // a key the trie holds, and it is that key's value.
    LIMITED_WHILE(fdp.ConsumeBool(), 8) {
        Bytes key{ConsumeRandomLengthByteVector<unsigned char>(fdp, 6)};
        if (!entries.empty() && fdp.ConsumeBool()) {
            key = std::next(entries.begin(), fdp.ConsumeIntegralInRange<size_t>(0, entries.size() - 1))->first;
            // Or a key one nibble or byte away from it.
            if (fdp.ConsumeBool() && !key.empty()) key.back() ^= fdp.ConsumeBool() ? 0x01 : 0x10;
            if (fdp.ConsumeBool()) key.push_back(fdp.ConsumeIntegral<uint8_t>());
            if (fdp.ConsumeBool() && !key.empty()) key.pop_back();
        }
        std::vector<Bytes> proof{trie.Proof(fdp.ConsumeBool() ? key : ConsumeRandomLengthByteVector<unsigned char>(fdp, 6))};
        if (!proof.empty()) {
            const size_t i{fdp.ConsumeIntegralInRange<size_t>(0, proof.size() - 1)};
            const size_t j{fdp.ConsumeIntegralInRange<size_t>(0, proof.size() - 1)};
            switch (fdp.ConsumeIntegralInRange<int>(0, 3)) {
            case 0: break;
            case 1: proof.erase(proof.begin() + i); break;
            case 2: {
                const Bytes repeated{proof[j]};
                proof.insert(proof.begin() + i, repeated);
                break;
            }
            case 3: std::swap(proof[i], proof[j]); break;
            }
        }
        if (const auto got{bridge::VerifyMptProof(root, key, proof)}) {
            const auto it{entries.find(key)};
            assert(it != entries.end() && *got == it->second);
        }
    }
}

FUZZ_TARGET(bridge_receipt)
{
    FuzzedDataProvider fdp{buffer.data(), buffer.size()};
    const bridge::EthAddress vault{ConsumeArray<20>(fdp)};
    // The V1 bound, the V2 bound, or any (above 0x7f it is clamped).
    const uint8_t max_type{fdp.PickValueInArray<uint8_t>(
        {bridge::RECEIPT_MAX_TYPE_V1, bridge::RECEIPT_MAX_TYPE_V2, fdp.ConsumeIntegral<uint8_t>()})};

    if (fdp.ConsumeBool()) {
        // Any bytes: never crash, and decode only from their canonical
        // encoding; deposits decode only from vault logs of their own shape.
        const Bytes bytes{fdp.ConsumeRemainingBytes<unsigned char>()};
        const auto receipt{bridge::DecodeReceipt(bytes, max_type)};
        if (!receipt) return;
        assert(receipt->type <= std::min(max_type, bridge::RECEIPT_MAX_TYPE_V2));
        const auto body{bridge::RlpDecode(std::span{bytes}.subspan(receipt->type == 0 ? 0 : 1))};
        assert(body);
        const auto fields{bridge::RlpChildren(*body)};
        assert(fields && fields->size() == 4);
        // Shapes the re-encoding below cannot see: the bloom's size and the
        // number of topics.
        assert(!(*fields)[2].is_list && (*fields)[2].payload.size() == 256);
        for (const bridge::EthLog& log : receipt->logs) assert(log.topics.size() <= 4);
        assert(EncodeReceipt(receipt->type, ReceiptFields(*receipt, (*fields)[2].payload)) == bytes);
        CheckDeposits(*receipt, vault);
        return;
    }

    // A receipt built as Ethereum writes it, holding vault deposits among
    // vault logs one field short of a deposit and other contracts' logs,
    // decodes to itself and yields exactly those deposits.
    bridge::EthReceipt receipt;
    // Any EIP-2718 type, within the bound or not.
    if (fdp.ConsumeBool()) receipt.type = fdp.ConsumeIntegralInRange<uint8_t>(1, bridge::RECEIPT_MAX_TYPE_V2);
    receipt.status = fdp.ConsumeBool();
    receipt.cumulative_gas = fdp.ConsumeIntegral<uint64_t>();
    std::vector<bridge::DepositEvent> deposits;
    LIMITED_WHILE(fdp.ConsumeBool(), 8) {
        const int kind{fdp.ConsumeIntegralInRange<int>(0, 2)};
        if (kind < 2) {
            bridge::DepositEvent event;
            event.deposit_id = fdp.ConsumeIntegral<uint64_t>();
            event.token = ConsumeArray<20>(fdp);
            event.amount = ConsumeArray<32>(fdp);
            event.b3_recipient = ConsumeArray<32>(fdp);
            bridge::EthLog log{EncodeDepositLog(event, vault)};
            if (kind == 0) {
                receipt.logs.push_back(std::move(log));
                deposits.push_back(event);
                continue;
            }
            // One field away from a deposit: never one.
            const uint8_t flip{fdp.ConsumeIntegralInRange<uint8_t>(1, 255)};
            switch (fdp.ConsumeIntegralInRange<int>(0, 4)) {
            case 0: log.topics[1].data()[fdp.ConsumeIntegralInRange<size_t>(0, 23)] ^= flip; break; // id padding
            case 1: log.topics[2].data()[fdp.ConsumeIntegralInRange<size_t>(0, 11)] ^= flip; break; // token padding
            case 2: log.topics[0].data()[fdp.ConsumeIntegralInRange<size_t>(0, 31)] ^= flip; break; // event signature
            case 3: // amount and recipient: not 64 bytes
                if (fdp.ConsumeBool()) {
                    log.data.push_back(flip);
                } else {
                    log.data.pop_back();
                }
                break;
            case 4: // not three topics
                if (fdp.ConsumeBool()) {
                    log.topics.push_back(ConsumeUInt256(fdp));
                } else {
                    log.topics.pop_back();
                }
                break;
            }
            receipt.logs.push_back(std::move(log));
        } else {
            // Another contract's log: never a vault deposit.
            bridge::EthLog log;
            log.address = ConsumeArray<20>(fdp);
            if (log.address == vault) log.address[0] ^= 1;
            LIMITED_WHILE(fdp.ConsumeBool(), 4) log.topics.push_back(ConsumeUInt256(fdp));
            log.data = ConsumeRandomLengthByteVector<unsigned char>(fdp, 200);
            receipt.logs.push_back(std::move(log));
        }
    }
    const auto bloom_bytes{ConsumeArray<256>(fdp)};
    const Bytes bloom(bloom_bytes.begin(), bloom_bytes.end());
    const Bytes bytes{EncodeReceipt(receipt.type, ReceiptFields(receipt, bloom))};
    const bool typed_ok{receipt.type <= std::min(max_type, bridge::RECEIPT_MAX_TYPE_V2)};
    const auto decoded{bridge::DecodeReceipt(bytes, max_type)};
    assert(decoded.has_value() == typed_ok);
    if (!decoded) return;
    assert(decoded->type == receipt.type && decoded->status == receipt.status &&
           decoded->cumulative_gas == receipt.cumulative_gas);
    assert(std::ranges::equal(decoded->logs, receipt.logs, SameLog));
    CheckDeposits(*decoded, vault);
    const auto found{bridge::ExtractDeposits(*decoded, vault)};
    if (receipt.status) {
        assert(std::ranges::equal(found, deposits, SameDeposit));
    } else {
        assert(found.empty());
    }

    // The same receipt with one field of another shape never decodes: a
    // field more or fewer, a status other than 0 or 1, a bloom a byte short
    // or long or in a list, a gas field that is no canonical integer, or
    // logs that are no list.
    std::vector<Bytes> fields{ReceiptFields(receipt, bloom)};
    switch (fdp.ConsumeIntegralInRange<int>(0, 5)) {
    case 0: fields.push_back(RandomRlp(fdp, 1)); break;
    case 1: fields.erase(fields.begin() + fdp.ConsumeIntegralInRange<size_t>(0, fields.size() - 1)); break;
    case 2: {
        Bytes status{fdp.ConsumeIntegral<uint8_t>()};
        if (status[0] == 0x01 || fdp.ConsumeBool()) status.push_back(fdp.ConsumeIntegral<uint8_t>());
        fields[0] = fdp.ConsumeBool() ? bridge::RlpEncodeBytes(status) : bridge::RlpEncodeList({});
        break;
    }
    case 3: {
        Bytes other{bloom};
        if (fdp.ConsumeBool()) {
            other.pop_back();
        } else {
            other.push_back(fdp.ConsumeIntegral<uint8_t>());
        }
        fields[2] = fdp.ConsumeBool() ? bridge::RlpEncodeBytes(other) : bridge::RlpEncodeList({fields[2]});
        break;
    }
    case 4: {
        Bytes gas;
        for (uint64_t rest{receipt.cumulative_gas}; rest != 0; rest >>= 8) {
            gas.insert(gas.begin(), static_cast<unsigned char>(rest));
        }
        if (fdp.ConsumeBool()) {
            gas.insert(gas.begin(), 0x00);
        } else {
            gas.insert(gas.begin(), 9 - gas.size(), 0x01);
        }
        fields[1] = bridge::RlpEncodeBytes(gas);
        break;
    }
    case 5: fields[3] = bridge::RlpEncodeBytes(ConsumeRandomLengthByteVector<unsigned char>(fdp, 8)); break;
    }
    assert(!bridge::DecodeReceipt(EncodeReceipt(receipt.type, fields), max_type));
}

FUZZ_TARGET(bridge_exec_ancestry)
{
    FuzzedDataProvider fdp{buffer.data(), buffer.size()};
    if (fdp.ConsumeBool()) {
        // Any headers, with the proven hash mostly the first one's so that
        // the walk gets past its first check: never crash, and return only
        // the last header, at the target, below the proven hash.
        std::vector<Bytes> headers;
        LIMITED_WHILE(fdp.remaining_bytes() > 0, 4) {
            headers.push_back(fdp.ConsumeBool() ? RandomRlp(fdp, 0)
                                                : ConsumeRandomLengthByteVector<unsigned char>(fdp, 600));
        }
        const uint256 proven{headers.empty() || fdp.ConsumeBool() ? ConsumeUInt256(fdp)
                                                                  : bridge_test::EthTrie::HashOf(headers[0])};
        const uint64_t target{fdp.ConsumeIntegral<uint64_t>()};
        if (const auto got{bridge::VerifyExecAncestry(proven, target, headers)}) {
            assert(bridge_test::EthTrie::HashOf(headers.front()) == proven);
            assert(got->block_number == target && got->block_hash == bridge_test::EthTrie::HashOf(headers.back()));
        }
        return;
    }

    // A chain numbered down by one from its newest header, except where a
    // header is numbered off its place. Walking from the newest header's
    // hash to a target returns the target's header exactly when the walk is
    // given the chain's own headers down to the target, each one clean.
    const uint64_t newest{fdp.ConsumeIntegralInRange<uint64_t>(64, std::numeric_limits<uint64_t>::max() - 64)};
    std::vector<uint64_t> numbers{newest};
    LIMITED_WHILE(fdp.ConsumeBool(), 7) {
        const uint64_t back{numbers.back()};
        numbers.push_back(fdp.ConsumeIntegralInRange<int>(0, 7) == 0
                              ? fdp.PickValueInArray<uint64_t>({back, back + 1, back - 2, back - 3})
                              : back - 1);
    }
    const std::vector<ChainHeader> chain{BuildChain(fdp, numbers)};
    const size_t k{fdp.ConsumeIntegralInRange<size_t>(0, chain.size() - 1)};
    std::vector<Bytes> candidate;
    candidate.reserve(k + 2);
    for (size_t h{0}; h <= k; ++h) candidate.push_back(chain[h].rlp);
    // Or not quite those headers.
    const size_t i{fdp.ConsumeIntegralInRange<size_t>(0, k)};
    const size_t j{fdp.ConsumeIntegralInRange<size_t>(0, k)};
    switch (fdp.ConsumeIntegralInRange<int>(0, 7)) {
    case 0: break;
    case 1: candidate.pop_back(); break; // ends above the target
    case 2: candidate.push_back(k + 1 < chain.size() ? chain[k + 1].rlp : RandomRlp(fdp, 0)); break; // one more
    case 3: candidate.erase(candidate.begin() + i); break;                                          // a link missing
    case 4: std::swap(candidate[i], candidate[j]); break;
    case 5: {
        Bytes& rlp{candidate[i]};
        rlp[fdp.ConsumeIntegralInRange<size_t>(0, rlp.size() - 1)] ^= fdp.ConsumeIntegralInRange<uint8_t>(1, 255);
        break;
    }
    case 6: {
        const Bytes repeated{candidate[j]};
        candidate.insert(candidate.begin() + i, repeated);
        break;
    }
    case 7: candidate[i] = chain[fdp.ConsumeIntegralInRange<size_t>(0, chain.size() - 1)].rlp; break;
    }
    const uint256 proven{fdp.ConsumeIntegralInRange<int>(0, 15) == 0 ? ConsumeUInt256(fdp)
                                                                     : chain[0].ancestor.block_hash};
    const uint64_t target{numbers[k]};
    const auto got{bridge::VerifyExecAncestry(proven, target, candidate)};

    // Headers 0 to m each parse, and count down by one.
    const auto clean{[&](const size_t m) {
        for (size_t h{0}; h <= m; ++h) {
            if (!chain[h].parses || (h > 0 && numbers[h] != numbers[h - 1] - 1)) return false;
        }
        return true;
    }};
    // The candidate is the chain's headers 0 to m (m < chain.size()).
    const auto prefix{[&](const size_t m) {
        return candidate.size() == m + 1 &&
               std::equal(candidate.begin(), candidate.end(), chain.begin(),
                          [](const Bytes& rlp, const ChainHeader& header) { return rlp == header.rlp; });
    }};
    if (proven == chain[0].ancestor.block_hash && prefix(k) && clean(k)) {
        assert(got && SameAncestor(*got, chain[k].ancestor));
    }
    if (got) {
        // Only ever the chain's own clean headers, from the proven hash down
        // to the target, with nothing after it.
        const size_t m{candidate.size() - 1};
        assert(proven == chain[0].ancestor.block_hash && m < chain.size() && prefix(m));
        assert(numbers[m] == target && clean(m) && SameAncestor(*got, chain[m].ancestor));
    }
}

FUZZ_TARGET(bridge_admission)
{
    FuzzedDataProvider fdp{buffer.data(), buffer.size()};
    // A Deposit's recipient word decodes only from the one encoding of a
    // P2PKH hash, and never to the all-zero hash.
    std::array<unsigned char, 32> word{ConsumeArray<32>(fdp)};
    const auto hash_begin{word.begin() + bridge::RECIPIENT_V1_PADDING_SIZE + 1};
    if (fdp.ConsumeBool()) {
        std::fill(word.begin(), word.begin() + bridge::RECIPIENT_V1_PADDING_SIZE, 0);
        word[bridge::RECIPIENT_V1_PADDING_SIZE] = bridge::RECIPIENT_V1_P2PKH;
        if (fdp.ConsumeBool()) std::fill(hash_begin, word.end(), 0);
    }
    const auto zero{[](const unsigned char byte) { return byte == 0; }};
    const bool shaped{std::all_of(word.begin(), word.begin() + bridge::RECIPIENT_V1_PADDING_SIZE, zero) &&
                      word[bridge::RECIPIENT_V1_PADDING_SIZE] == bridge::RECIPIENT_V1_P2PKH};
    const bool zero_hash{std::all_of(hash_begin, word.end(), zero)};
    const auto recipient{bridge::DecodeRecipientV1(word)};
    assert(recipient.has_value() == (shaped && !zero_hash));
    if (recipient) assert(bridge::EncodeRecipientV1(*recipient) == word);

    // A Deposit's amount converts between decimals as 256-bit integers
    // would. Mostly a mantissa times a power of ten, at times just off it;
    // or just around the money bound at some scale; or any word.
    std::array<unsigned char, 32> amount{};
    switch (fdp.ConsumeIntegralInRange<int>(0, 2)) {
    case 0:
        amount = ConsumeArray<32>(fdp);
        break;
    case 1: {
        arith_uint256 value{fdp.ConsumeIntegral<uint64_t>()};
        value *= PowerOfTen(fdp.ConsumeIntegralInRange<unsigned>(0, 40));
        if (fdp.ConsumeBool()) value += fdp.ConsumeIntegralInRange<uint64_t>(1, 9);
        amount = ToBigEndian(value);
        break;
    }
    case 2: {
        const unsigned scale{fdp.ConsumeIntegralInRange<unsigned>(0, 18)};
        const int64_t bound{MAX_MONEY / static_cast<int64_t>(PowerOfTen(scale).GetLow64())};
        const int64_t around{std::max<int64_t>(0, bound + fdp.ConsumeIntegralInRange<int64_t>(-300, 300))};
        arith_uint256 value{static_cast<uint64_t>(around)};
        value *= PowerOfTen(fdp.ConsumeIntegralInRange<unsigned>(0, 18));
        amount = ToBigEndian(value);
        break;
    }
    }
    const uint8_t origin{fdp.ConsumeIntegralInRange<uint8_t>(0, 20)};
    const uint8_t asset{fdp.ConsumeIntegralInRange<uint8_t>(0, 20)};
    assert(bridge::ConvertRawUnitsExact(amount, origin, asset) == ReferenceConvert(amount, origin, asset));
}
