// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! The modern transaction encoding (TX_MODERN), in which a node reads every
//! modern-era transaction from its peers and from blocks. It adds the Modern
//! Payload Area, the section that carries bridge records and the other typed
//! payloads, to the upstream witness encoding, which is all the upstream
//! transaction target reads. A transaction decodes only from its own
//! canonical encoding; without a payload area its modern and upstream
//! encodings are the same bytes, and the upstream encoding refuses a payload
//! area. Mainnet's per-transaction payload rules then run on what decoded.

#include <bridge/proof.h>
#include <chainparams.h>
#include <consensus/consensus.h>
#include <consensus/tx_check.h>
#include <consensus/validation.h>
#include <modern/bridge_binding.h>
#include <modern/mpa.h>
#include <primitives/transaction.h>
#include <streams.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <util/chaintype.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <ios>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace {

using Bytes = std::vector<unsigned char>;

void initialize_modern_transaction() { SelectParams(ChainType::MAIN); }

bool InStrictOrder(const CMpaRecord& a, const CMpaRecord& b)
{
    return std::tie(a.payload_type, a.payload_version, a.payload) <
           std::tie(b.payload_type, b.payload_version, b.payload);
}

//! If `bytes` decode as a modern transaction: they are its canonical
//! encoding, the upstream encoding reads the same transaction exactly when
//! it has no payload area, and mainnet's payload rules accept only records
//! in strict order with a bridge record alone, bound by its one exact
//! output and decoding under the limits the rules apply at that height (the
//! same under the V2 limits when the V1 limits accept it).
void CheckModernBytes(const std::span<const unsigned char> bytes)
{
    CMutableTransaction mtx;
    SpanReader reader{bytes};
    try {
        reader >> TX_MODERN(mtx);
    } catch (const std::ios_base::failure&) {
        return;
    }
    const auto consumed{bytes.first(bytes.size() - reader.size())};
    DataStream encoded;
    encoded << TX_MODERN(mtx);
    assert(std::ranges::equal(MakeUCharSpan(encoded), consumed));

    CMutableTransaction upstream;
    bool upstream_reads{true};
    try {
        SpanReader{consumed} >> TX_WITH_WITNESS(upstream);
    } catch (const std::ios_base::failure&) {
        upstream_reads = false;
    }
    if (mtx.mpa.empty()) {
        assert(upstream_reads);
        DataStream reread;
        reread << TX_WITH_WITNESS(upstream);
        assert(std::ranges::equal(MakeUCharSpan(reread), consumed));
    } else {
        assert(!upstream_reads);
        // The payload area adds its section, and the extended format's
        // marker and flags when no witness brings them.
        DataStream witness;
        witness << TX_WITH_WITNESS(mtx);
        assert(encoded.size() ==
               witness.size() + GetMpaSectionSerializedSize(mtx.mpa) + (mtx.HasWitness() ? 0 : 2));
    }

    const CTransaction tx{mtx};
    TxValidationState state;
    if (!CheckTransaction(tx, state)) return; // the payload rules run only after these
    const Consensus::Params& params{Params().GetConsensus()};
    for (const std::optional<int> height : {std::optional<int>{}, std::optional<int>{0},
                                            std::optional<int>{std::numeric_limits<int>::max()}}) {
        std::string error;
        if (!modern::CheckTransactionMpa(tx, params, height, error)) continue;
        size_t bridge_records{0};
        for (size_t i{0}; i < tx.mpa.size(); ++i) {
            const CMpaRecord& record{tx.mpa[i]};
            if (i > 0) assert(InStrictOrder(tx.mpa[i - 1], record));
            if (record.payload_type != modern::MPA_TYPE_BRIDGE) continue;
            ++bridge_records;
            const bool v2{height && params.BridgeV2RulesActive(*height)};
            assert(bridge::DecodeBridgeMpaRecordV1(record, v2 ? bridge::BRIDGE_MPT_LIMITS_V2 : bridge::BRIDGE_MPT_LIMITS_V1));
            if (const auto v1{bridge::DecodeBridgeMpaRecordV1(record)}) {
                assert(bridge::DecodeBridgeMpaRecordV1(record, bridge::BRIDGE_MPT_LIMITS_V2) == v1);
            }
            const auto binding{modern::MakeBridgeBindingOutput(record)};
            assert(binding && std::ranges::count(tx.vout, *binding) == 1);
        }
        assert(bridge_records == 0 || tx.mpa.size() == 1);
        assert(modern::BridgeBindingOutputCount(tx) == bridge_records);
    }
}

//! A payload record of a registered type, or any type, with a payload of a
//! size some type requires, or any small size.
CMpaRecord Record(FuzzedDataProvider& fdp)
{
    CMpaRecord record;
    record.payload_type = fdp.PickValueInArray<uint16_t>(
        {modern::CREATION_ACTION_ASSET_ISSUANCE, modern::CREATION_ACTION_MODERN_FN_POD,
         modern::CREATION_ACTION_FLOWMESH_SEAT_BINDING, modern::MPA_TYPE_FINALITY_CERTIFICATE,
         modern::MPA_TYPE_FINALITY_KEY_EVIDENCE, modern::MPA_TYPE_FLOWMESH_CHECKPOINT,
         modern::MPA_TYPE_FLOWMESH_VAULT_PROOF, modern::MPA_TYPE_BRIDGE, modern::MPA_TYPE_FINALITY_CERTIFICATE_V2,
         modern::MPA_TYPE_FRESH_FINALITY_KEY_EVIDENCE, fdp.ConsumeIntegral<uint16_t>()});
    record.payload_version = fdp.ConsumeBool() ? modern::MPA_VERSION_V1 : fdp.ConsumeIntegral<uint16_t>();
    if (fdp.ConsumeBool()) {
        record.payload = ConsumeRandomLengthByteVector<unsigned char>(fdp, 160);
    } else {
        const size_t size{fdp.PickValueInArray<size_t>({modern::FINALITY_KEY_EVIDENCE_SIZE,
                                                        modern::MODERN_FN_POD_ACTION_V1_SIZE,
                                                        modern::FLOWMESH_SEAT_BINDING_ACTION_V1_SIZE,
                                                        modern::FRESH_FINALITY_KEY_EVIDENCE_SIZE})};
        record.payload.assign(size, fdp.ConsumeIntegral<uint8_t>());
    }
    return record;
}

} // namespace

FUZZ_TARGET(modern_transaction, .init = initialize_modern_transaction)
{
    FuzzedDataProvider fdp{buffer.data(), buffer.size()};
    if (fdp.ConsumeBool()) {
        CheckModernBytes(fdp.ConsumeRemainingBytes<unsigned char>());
        return;
    }

    // A transaction with a payload area of registered types, now and then a
    // record twice or out of order, and at times a bridge record with its
    // binding output (or another record's). Encoded as a node writes it, it
    // decodes back to itself; so do the same bytes with one byte changed,
    // cut off or added, when they decode at all.
    CMutableTransaction mtx;
    mtx.version = fdp.ConsumeBool() ? CTransaction::CURRENT_VERSION : fdp.ConsumeIntegral<uint32_t>();
    LIMITED_WHILE(mtx.vin.empty() || fdp.ConsumeBool(), 3) {
        CTxIn in{COutPoint{Txid::FromUint256(ConsumeUInt256(fdp)), fdp.ConsumeIntegral<uint32_t>()},
                 ConsumeScript(fdp), fdp.ConsumeIntegral<uint32_t>()};
        if (fdp.ConsumeBool()) in.scriptWitness = ConsumeScriptWitness(fdp);
        mtx.vin.push_back(std::move(in));
    }
    LIMITED_WHILE(mtx.vout.empty() || fdp.ConsumeBool(), 3) {
        const CAmount value{ConsumeMoney(fdp)};
        mtx.vout.emplace_back(value, ConsumeScript(fdp));
    }
    LIMITED_WHILE(fdp.ConsumeBool(), 4) {
        mtx.mpa.push_back(Record(fdp));
        if (fdp.ConsumeIntegralInRange<int>(0, 7) == 0) mtx.mpa.push_back(mtx.mpa.back());
    }
    if (fdp.ConsumeBool()) {
        CMpaRecord record{modern::MPA_TYPE_BRIDGE, modern::MPA_VERSION_V1,
                          ConsumeRandomLengthByteVector<unsigned char>(fdp, 200)};
        if (fdp.ConsumeBool()) {
            // A record that decodes: a burn or managed withdrawal, kind,
            // reserved byte, registry id, output, amount and recipient.
            record.payload = {fdp.PickValueInArray<unsigned char>({5, 6}), 0};
            const Bytes body{fdp.ConsumeBytes<unsigned char>(64)};
            record.payload.insert(record.payload.end(), body.begin(), body.end());
            record.payload.resize(66);
            record.payload[2] |= 1;  // registry id
            record.payload[45] |= 1; // amount
            record.payload[46] |= 1; // recipient
        }
        CMpaRecord bound{record};
        if (fdp.ConsumeIntegralInRange<int>(0, 7) == 0) bound.payload.push_back(0); // another record's binding
        if (const auto binding{modern::MakeBridgeBindingOutput(bound)}) {
            mtx.vout.insert(mtx.vout.begin() + fdp.ConsumeIntegralInRange<size_t>(0, mtx.vout.size()), *binding);
        }
        mtx.mpa.push_back(std::move(record));
    }
    if (fdp.ConsumeBool()) std::sort(mtx.mpa.begin(), mtx.mpa.end(), InStrictOrder);

    DataStream stream;
    stream << TX_MODERN(mtx);
    const auto span{MakeUCharSpan(stream)};
    const Bytes bytes(span.begin(), span.end());
    if (mtx.mpa.size() <= MAX_PAYLOAD_RECORDS_PER_TX) {
        CMutableTransaction back;
        SpanReader reader{bytes};
        try {
            reader >> TX_MODERN(back);
        } catch (const std::ios_base::failure&) {
            assert(false);
        }
        assert(reader.empty());
    }
    CheckModernBytes(bytes);
    Bytes damaged{bytes};
    const size_t at{fdp.ConsumeIntegralInRange<size_t>(0, damaged.size() - 1)};
    switch (fdp.ConsumeIntegralInRange<int>(0, 2)) {
    case 0: damaged[at] ^= fdp.ConsumeIntegralInRange<uint8_t>(1, 255); break;
    case 1: damaged.resize(at); break;
    case 2: damaged.insert(damaged.begin() + at, fdp.ConsumeIntegral<uint8_t>()); break;
    }
    CheckModernBytes(damaged);
}
