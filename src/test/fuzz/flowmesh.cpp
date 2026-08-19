// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! FlowMesh decoder and candidate-execution fuzzing: the action,
//! microblock, certificate and state-snapshot codecs must never crash,
//! leak reads past their input, or break round-trip identity on
//! adversarial bytes; candidate re-execution of arbitrary decoded
//! microblocks must never mutate the committed state.

#include <flowmesh/batch.h>
#include <flowmesh/certificate.h>
#include <flowmesh/microblock.h>
#include <flowmesh/state.h>
#include <flowmesh/sync.h>
#include <streams.h>
#include <test/fuzz/fuzz.h>
#include <uint256.h>

#include <optional>
#include <vector>

namespace {

const uint256 FUZZ_VAULT{uint256::ONE};
const uint256 FUZZ_DOMAIN{uint256::ONE};

modern::AssetId FuzzBase()
{
    uint256 id;
    id.data()[0] = 0x11;
    return id;
}

class PassAuth final : public flowmesh::ActionAuthenticator
{
public:
    bool Authenticate(const flowmesh::Action&) const override { return true; }
};

template <typename T>
std::optional<T> Decode(DataStream& s)
{
    T obj;
    try {
        s >> obj;
    } catch (const std::exception&) {
        return std::nullopt;
    }
    return obj;
}

} // namespace

FUZZ_TARGET(flowmesh_action_codec)
{
    DataStream s{MakeByteSpan(buffer)};
    const auto action{Decode<flowmesh::Action>(s)};
    if (!action) return;

    // Pure functions are total on any decoded value.
    const uint256 id{action->Id()};
    (void)action->ShapeIsCanonical();

    // Round trip: re-encoding what decoded must reproduce the identity.
    DataStream again;
    again << *action;
    const auto second{Decode<flowmesh::Action>(again)};
    assert(second);
    assert(again.empty());
    assert(second->Id() == id);
}

FUZZ_TARGET(flowmesh_microblock_codec)
{
    DataStream s{MakeByteSpan(buffer)};
    const auto mb{Decode<flowmesh::MicroblockCore>(s)};
    if (!mb) return;

    const uint256 hash{mb->GetHash()};
    (void)mb->ShapeIsValid();
    (void)flowmesh::MicroblockCore::ComputeActionsRoot(mb->actions);

    DataStream again;
    again << *mb;
    const auto second{Decode<flowmesh::MicroblockCore>(again)};
    assert(second);
    assert(again.empty());
    assert(second->GetHash() == hash);

    // Candidate re-execution of arbitrary decoded microblocks: total,
    // and atomic — a rejected (or even accepted) candidate never
    // mutates the state it ran against.
    flowmesh::FlowMeshState state{FUZZ_VAULT, FuzzBase(), modern::NativeAsset()};
    const uint256 before{state.Root()};
    const PassAuth auth;
    flowmesh::FlowMeshState next{state};
    flowmesh::BatchResult result;
    (void)flowmesh::ExecuteCandidate(state, FUZZ_DOMAIN, uint256{}, *mb, auth,
                                     /*deposits=*/nullptr, next, result);
    assert(state.Root() == before);
}

FUZZ_TARGET(flowmesh_certificate_codec)
{
    DataStream s{MakeByteSpan(buffer)};
    const auto entry{Decode<flowmesh::CertifiedEntry>(s)};
    if (!entry) return;

    // Certificate checking with an empty seat set must refuse totally
    // and safely (no seats -> nothing verifies) on any decoded bytes.
    const auto check{flowmesh::CheckCertificate(entry->cert, FUZZ_DOMAIN, {}, 1)};
    assert(check != flowmesh::CertificateCheck::OK);

    // Canonical assembly is total and idempotent on decoded attestations.
    const flowmesh::MicroblockCertificate assembled{flowmesh::AssembleCertificate(
        entry->cert.microblock_hash, entry->cert.sequence, entry->cert.attestations)};
    for (size_t i{1}; i < assembled.attestations.size(); ++i) {
        assert(assembled.attestations[i - 1].validator < assembled.attestations[i].validator);
    }
}

FUZZ_TARGET(flowmesh_state_snapshot)
{
    DataStream s{MakeByteSpan(buffer)};
    flowmesh::FlowMeshState state{FUZZ_VAULT, FuzzBase(), modern::NativeAsset()};
    try {
        s >> state;
    } catch (const std::exception&) {
        return; // refused decode leaves no obligation
    }
    // Whatever decoded (before certificate verification would judge it),
    // the state's pure functions are total.
    (void)state.Root();
    (void)state.ledger.SolvencyHolds();
}
