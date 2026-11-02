// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! B0b derived-state codecs: a persisted record, and the finality binding
//! index persisted in one, must never throw and must decode only from
//! exactly one encoding. Random bytes almost never pass the record's
//! checksum or the index's replay checks, so each target also builds a
//! valid value, checks its round trip, and checks that no damaged copy of
//! its encoding decodes as the value it came from.

#include <modern/finality_key.h>
#include <node/derived_state.h>
#include <node/finality_binding_index.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <uint256.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

using node::DerivedStateCheck;
using node::DerivedStateRecord;
using node::FinalityBindingIndex;

namespace {

DerivedStateCheck DecodeRecord(const std::vector<unsigned char>& bytes, const std::string& build_id,
                               const uint256& params_digest)
{
    DerivedStateRecord record;
    return node::DecodeDerivedStateRecord(bytes, build_id, params_digest, record);
}

//! One record in an encoded index: validator key, BLS key, seq, height.
constexpr size_t BINDING_ENTRY_SIZE{sizeof(modern::ValidatorKeyBytes) + sizeof(modern::BlsPubkeyBytes) +
                                    sizeof(uint32_t) + sizeof(int32_t)};

} // namespace

FUZZ_TARGET(derived_state_record)
{
    FuzzedDataProvider fdp{buffer.data(), buffer.size()};
    // Real build ids are a version and a commit hash, far below the
    // decoder's 256-byte bound.
    const std::string build_id{fdp.ConsumeRandomLengthString(256)};
    const uint256 params_digest{ConsumeUInt256(fdp)};

    if (fdp.ConsumeBool()) {
        // Any bytes: never throws, and decodes only from its own encoding.
        const std::vector<unsigned char> bytes{fdp.ConsumeRemainingBytes<unsigned char>()};
        DerivedStateRecord record;
        const DerivedStateCheck check{node::DecodeDerivedStateRecord(bytes, build_id, params_digest, record)};
        if (!bytes.empty() && bytes[0] != 1) assert(check == DerivedStateCheck::OTHER_FORMAT);
        if (check == DerivedStateCheck::OK) assert(node::EncodeDerivedStateRecord(record, build_id) == bytes);
        return;
    }

    DerivedStateRecord record;
    record.params_digest = params_digest;
    record.block_hash = ConsumeUInt256(fdp);
    record.height = fdp.ConsumeIntegral<int>();
    record.payload = ConsumeRandomLengthByteVector<unsigned char>(fdp, 4096);
    const std::vector<unsigned char> bytes{node::EncodeDerivedStateRecord(record, build_id)};

    DerivedStateRecord decoded;
    assert(node::DecodeDerivedStateRecord(bytes, build_id, params_digest, decoded) == DerivedStateCheck::OK);
    assert(decoded.params_digest == record.params_digest);
    assert(decoded.block_hash == record.block_hash);
    assert(decoded.height == record.height);
    assert(decoded.payload == record.payload);

    // Another build's record, or one written under other parameters, is
    // never used.
    assert(DecodeRecord(bytes, build_id + "+", params_digest) == DerivedStateCheck::OTHER_BUILD);
    uint256 other_digest{params_digest};
    other_digest.data()[fdp.ConsumeIntegralInRange<size_t>(0, uint256::size() - 1)] ^= 1;
    assert(DecodeRecord(bytes, build_id, other_digest) == DerivedStateCheck::OTHER_PARAMS);

    // A damaged copy is never used: one byte changed, cut short or extended.
    std::vector<unsigned char> changed{bytes};
    changed[fdp.ConsumeIntegralInRange<size_t>(0, bytes.size() - 1)] ^= fdp.ConsumeIntegralInRange<uint8_t>(1, 255);
    assert(DecodeRecord(changed, build_id, params_digest) != DerivedStateCheck::OK);
    const std::vector<unsigned char> cut{bytes.begin(),
                                         bytes.begin() + fdp.ConsumeIntegralInRange<size_t>(0, bytes.size() - 1)};
    assert(DecodeRecord(cut, build_id, params_digest) != DerivedStateCheck::OK);
    std::vector<unsigned char> extended{bytes};
    extended.push_back(fdp.ConsumeIntegral<uint8_t>());
    assert(DecodeRecord(extended, build_id, params_digest) != DerivedStateCheck::OK);
}

FUZZ_TARGET(finality_binding_index_codec)
{
    FuzzedDataProvider fdp{buffer.data(), buffer.size()};

    if (fdp.ConsumeBool()) {
        // Any bytes: never throws, and decodes only from its own encoding.
        const int first{fdp.ConsumeIntegral<int>()};
        const int last{fdp.ConsumeIntegral<int>()};
        const std::vector<unsigned char> bytes{fdp.ConsumeRemainingBytes<unsigned char>()};
        if (const auto index{FinalityBindingIndex::Decode(bytes, first, last)}) assert(index->Encode() == bytes);
        return;
    }

    // Build an index as ConnectBlock does: validators from a small pool bind,
    // rebind and revoke keys, and no nonzero key is held by two validators
    // at once, which consensus refuses.
    FinalityBindingIndex index;
    std::map<modern::BlsPubkeyBytes, modern::ValidatorKeyBytes> owner;
    std::map<modern::ValidatorKeyBytes, modern::BlsPubkeyBytes> held;
    const int first{fdp.ConsumeIntegralInRange<int>(0, 1'000'000)};
    int height{first};
    LIMITED_WHILE(fdp.ConsumeBool(), 32) {
        std::vector<FinalityBindingIndex::Transition> transitions;
        const int count{fdp.ConsumeIntegralInRange<int>(0, 8)};
        for (int i{0}; i < count; ++i) {
            FinalityBindingIndex::Transition t;
            t.validator_key.fill(fdp.ConsumeIntegralInRange<uint8_t>(1, 8));
            // An all-zero key is a revocation.
            if (fdp.ConsumeBool()) t.record.bls_pubkey.fill(fdp.ConsumeIntegralInRange<uint8_t>(1, 8));
            t.record.seq = fdp.ConsumeIntegral<uint32_t>();
            t.record.height = height;
            if (!t.record.IsRevoked()) {
                const auto it{owner.find(t.record.bls_pubkey)};
                if (it != owner.end() && it->second != t.validator_key) continue;
            }
            if (const auto it{held.find(t.validator_key)}; it != held.end()) {
                owner.erase(it->second);
                held.erase(it);
            }
            if (!t.record.IsRevoked()) {
                owner[t.record.bls_pubkey] = t.validator_key;
                held[t.validator_key] = t.record.bls_pubkey;
            }
            transitions.push_back(t);
        }
        index.ConnectBlock(height, transitions);
        height += fdp.ConsumeIntegralInRange<int>(1, 1000);
    }
    const int last{std::max(first, index.ConnectedHeight())};

    const std::vector<unsigned char> bytes{index.Encode()};
    const auto decoded{FinalityBindingIndex::Decode(bytes, first, last)};
    assert(decoded);
    assert(decoded->SameState(index));
    assert(decoded->Encode() == bytes);

    if (index.ConnectedHeight() >= 0) {
        // The decoded undo trail reverts a block as the original does.
        FinalityBindingIndex original{index};
        FinalityBindingIndex restored{*decoded};
        original.DisconnectBlock(index.ConnectedHeight());
        restored.DisconnectBlock(index.ConnectedHeight());
        assert(restored.SameState(original));
        // A block outside the heights the caller allows is refused.
        assert(!FinalityBindingIndex::Decode(bytes, first + 1, last));
        assert(!FinalityBindingIndex::Decode(bytes, first, last - 1));
    }

    // Records out of key order are refused: an index has one encoding.
    if (index.Size() >= 2) {
        const size_t i{fdp.ConsumeIntegralInRange<size_t>(0, index.Size() - 2)};
        std::vector<unsigned char> swapped{bytes};
        const auto at{swapped.begin() + 1 + i * BINDING_ENTRY_SIZE};
        std::swap_ranges(at, at + BINDING_ENTRY_SIZE, at + BINDING_ENTRY_SIZE);
        assert(!FinalityBindingIndex::Decode(swapped, first, last));
    }

    // A damaged copy is refused, or is exactly the encoding of another
    // index; an extended one is refused.
    std::vector<unsigned char> changed{bytes};
    changed[fdp.ConsumeIntegralInRange<size_t>(0, bytes.size() - 1)] ^= fdp.ConsumeIntegralInRange<uint8_t>(1, 255);
    if (const auto other{FinalityBindingIndex::Decode(changed, first, last)}) assert(other->Encode() == changed);
    std::vector<unsigned char> extended{bytes};
    extended.push_back(fdp.ConsumeIntegral<uint8_t>());
    assert(!FinalityBindingIndex::Decode(extended, first, last));
}
