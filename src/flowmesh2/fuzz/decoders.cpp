// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** Fuzz targets for every flowmesh2 decoder (M0-FUZ-01).
 *
 * Each fm2 codec is exact: one encoding per value, and Decode refuses
 * anything else (core/codec.h). So every target checks the same oracle: an
 * input that decodes must encode back to exactly the input bytes, and must
 * decode again. A decoder that accepts a byte too many, reads a field at the
 * wrong offset or narrows a field breaks the oracle even when nothing
 * crashes. A cap that the decoder and the encoder both ignore keeps the
 * round trip intact, so the targets whose caps do not depend on a profile
 * also check the documented bounds of what Decode returns. Decoders that take
 * a profile get one of the regtest profiles, chosen by the first input byte.
 *
 * Run one target with FUZZ=<name> bin/fm2_fuzz [corpus dir]; see
 * src/flowmesh2/README.md.
 */

#include <flowmesh2/agreement/messages.h>
#include <flowmesh2/chain/chain_adapter.h>
#include <flowmesh2/core/account_leaf.h>
#include <flowmesh2/core/action.h>
#include <flowmesh2/core/body.h>
#include <flowmesh2/core/certificate.h>
#include <flowmesh2/core/facts.h>
#include <flowmesh2/core/outcome.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/slot.h>
#include <flowmesh2/core/smt.h>
#include <flowmesh2/core/state_root.h>
#include <flowmesh2/core/state_root_v1.h>
#include <flowmesh2/net/tcp_mesh_wire.h>
#include <flowmesh2/read/views.h>
#include <flowmesh2/state/snapshot.h>
#include <flowmesh2/storage/commit_log.h>
#include <flowmesh2/storage/control_log.h>
#include <flowmesh2/storage/record.h>
#include <flowmesh2/storage/signer_log.h>
#include <flowmesh2/storage/snapshot_mark.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <util/chaintype.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <span>
#include <vector>

using namespace flowmesh2;

namespace {

using Bytes = std::span<const unsigned char>;

const std::array<ProfileParams, 3>& Profiles()
{
    static const std::array<ProfileParams, 3> profiles{
        DevRegtestProfile(ChainType::REGTEST).value(),
        StableQViewRegtestProfile(ChainType::REGTEST).value(),
        StableQNetworkRegtestProfile(ChainType::REGTEST).value(),
    };
    return profiles;
}

void InitProfiles() { (void)Profiles(); }

//! The profile named by the first byte, and the rest of the input.
std::pair<const ProfileParams&, Bytes> WithProfile(FuzzBufferType buffer)
{
    if (buffer.empty()) return {Profiles()[0], Bytes{}};
    return {Profiles()[buffer[0] % Profiles().size()], Bytes{buffer.data() + 1, buffer.size() - 1}};
}

bool Same(const std::vector<unsigned char>& encoded, Bytes input) { return std::ranges::equal(encoded, input); }

/** The oracle: if `decode(bytes)` succeeds, encode(value) == bytes and
 * decode(encode(value)) succeeds. `encode` returns the bytes.
 */
template <class Decode, class Encode>
void RoundTrip(Bytes bytes, Decode&& decode, Encode&& encode)
{
    auto decoded{decode(bytes)};
    if (!decoded.has_value()) return;
    const std::vector<unsigned char> again{encode(*decoded)};
    assert(Same(again, bytes));
    assert(decode(Bytes{again}).has_value());
}

//! RoundTrip for a type with static Decode(bytes) and Encode().
template <class T>
void RoundTripPlain(Bytes bytes)
{
    RoundTrip(bytes, [](Bytes b) { return T::Decode(b); }, [](const T& v) { return v.Encode(); });
}

//! RoundTrip for a type with static Decode(bytes, profile) and Encode().
template <class T>
void RoundTripProfiled(FuzzBufferType buffer)
{
    const auto [profile, bytes]{WithProfile(buffer)};
    RoundTrip(bytes, [&](Bytes b) { return T::Decode(b, profile); }, [](const T& v) { return v.Encode(); });
}

} // namespace

FUZZ_TARGET(fm2_action, .init = InitProfiles) { RoundTripProfiled<ActionV2>(buffer); }
FUZZ_TARGET(fm2_slot_body, .init = InitProfiles) { RoundTripProfiled<SlotBody>(buffer); }
FUZZ_TARGET(fm2_outcome, .init = InitProfiles) { RoundTripProfiled<OutcomeV2>(buffer); }
FUZZ_TARGET(fm2_account_leaf, .init = InitProfiles)
{
    RoundTripProfiled<AccountLeafV0>(buffer);
    // core/account_leaf.h: at most MAX_LEAF_ASSETS balances and profile.nonce_window nonces.
    const auto [profile, bytes]{WithProfile(buffer)};
    if (const auto leaf{AccountLeafV0::Decode(bytes, profile)}) {
        assert(leaf->balances.size() <= MAX_LEAF_ASSETS);
        assert(leaf->used_nonces.size() <= profile.nonce_window);
    }
}
FUZZ_TARGET(fm2_smt_proof)
{
    const Bytes bytes{buffer.data(), buffer.size()};
    RoundTripPlain<SmtProof>(bytes);
    // core/smt.h: a decoded proof is canonical, so it folds to a root along any path its terminal allows.
    if (const auto proof{SmtProof::Decode(bytes)}) {
        assert(proof->depth <= SMT_DEPTH && proof->bitmap.size() == (proof->depth + 7) / 8u);
        assert(proof->siblings.size() <= proof->depth);
        const uint256 path{proof->terminal == SmtTerminal::LEAF ? proof->leaf.path : uint256{}};
        if (proof->terminal == SmtTerminal::LEAF) assert(SmtFoldRoot(path, *proof).has_value());
    }
}
FUZZ_TARGET(fm2_account_proof_v1, .init = InitProfiles)
{
    RoundTripProfiled<AccountProofV1>(buffer);
    // core/state_root_v1.h: PRESENT iff the proof ends at the account's own leaf.
    const auto [profile, bytes]{WithProfile(buffer)};
    if (const auto proof{AccountProofV1::Decode(bytes, profile)}) {
        const bool own{proof->proof.terminal == SmtTerminal::LEAF && proof->proof.leaf.path == AccountPathV1(proof->account)};
        assert(proof->leaf.has_value() == own);
        if (proof->leaf.has_value()) assert(proof->leaf->account == proof->account);
    }
}
FUZZ_TARGET(fm2_slot_header) { RoundTripPlain<SlotHeaderV2>(Bytes{buffer.data(), buffer.size()}); }
FUZZ_TARGET(fm2_slot_certificate)
{
    const Bytes bytes{buffer.data(), buffer.size()};
    RoundTripPlain<SlotCertificate>(bytes);
    // core/certificate.h: a bitmap longer than MAX_BITMAP_BYTES is OVERSIZE.
    if (const auto cert{SlotCertificate::Decode(bytes)}) assert(cert->bitmap.size() <= MAX_BITMAP_BYTES);
}
FUZZ_TARGET(fm2_certified_envelope)
{
    const Bytes bytes{buffer.data(), buffer.size()};
    RoundTripPlain<read::CertifiedEnvelope>(bytes);
    if (const auto envelope{read::CertifiedEnvelope::Decode(bytes)}) assert(envelope->cert.bitmap.size() <= MAX_BITMAP_BYTES);
}

FUZZ_TARGET(fm2_state_leaves)
{
    // The first byte picks the leaf: totals, resting order or withdrawal.
    if (buffer.empty()) return;
    const Bytes bytes{buffer.data() + 1, buffer.size() - 1};
    switch (buffer[0] % 3) {
    case 0: RoundTripPlain<TotalsLeafV0>(bytes); break;
    case 1: RoundTripPlain<RestingOrderLeafV0>(bytes); break;
    default: RoundTripPlain<WithdrawalRecord>(bytes); break;
    }
}

FUZZ_TARGET(fm2_fact)
{
    RoundTrip(Bytes{buffer.data(), buffer.size()}, [](Bytes b) { return DecodeFact(b); },
              [](const Fact& fact) { return EncodeFact(fact).value(); });
}

//! The agreement messages, which are also every lane frame's plaintext on
//! the seat mesh, control reports and NEW_VIEW bundles included.
FUZZ_TARGET(fm2_agreement_message, .init = InitProfiles)
{
    const auto [profile, bytes]{WithProfile(buffer)};
    RoundTrip(bytes, [&](Bytes b) { return agreement::DecodeMessage(b, profile); },
              [&](const agreement::Message& message) { return agreement::EncodeMessage(message, profile).value(); });
}

//! Journal record payloads, the first byte picking the record type: the
//! signer, commit and regtest control journal records.
FUZZ_TARGET(fm2_journal_payload, .init = InitProfiles)
{
    if (buffer.size() < 2) return;
    const ProfileParams& profile{Profiles()[buffer[1] % Profiles().size()]};
    const Bytes bytes{buffer.data() + 2, buffer.size() - 2};
    using namespace storage;
    switch (static_cast<RecordType>(buffer[0] % 12)) {
    case RecordType::INTENT: RoundTripPlain<IntentRecord>(bytes); break;
    case RecordType::SIGNED_VOTE: RoundTripPlain<SignedVoteRecord>(bytes); break;
    case RecordType::DECISION:
        RoundTrip(bytes, [&](Bytes b) { return DecisionRecord::Decode(b, profile); }, [](const DecisionRecord& r) { return r.Encode(); });
        break;
    case RecordType::APPLIED: RoundTripPlain<AppliedRecord>(bytes); break;
    case RecordType::SNAPSHOT_MARK: RoundTripPlain<SnapshotMark>(bytes); break;
    case RecordType::ACCEPTED_PROPOSAL:
        RoundTrip(bytes, [&](Bytes b) { return AcceptedProposalRecord::Decode(b, profile); },
                  [](const AcceptedProposalRecord& r) { return r.Encode(); });
        break;
    case RecordType::NEW_VIEW_ACCEPTED: RoundTripPlain<AcceptedControlView>(bytes); break;
    case RecordType::PREPARED_QC:
        RoundTrip(bytes, [&](Bytes b) { return PreparedQcRecord::Decode(b, profile); }, [](const PreparedQcRecord& r) { return r.Encode(); });
        break;
    case RecordType::CONTROL_INTENT: RoundTripPlain<ControlIntentRecord>(bytes); break;
    case RecordType::SIGNED_CONTROL: RoundTripPlain<SignedControlRecord>(bytes); break;
    default: break; // 0 and VIEW_STATE (7) have no payload codec
    }
}

/** A journal image (the record framing, CRC and chain): whatever ScanRecords
 * accepts re-frames to exactly the bytes it consumed, and only zero bytes
 * follow them.
 */
FUZZ_TARGET(fm2_journal_image)
{
    if (buffer.empty()) return;
    const auto role{(buffer[0] & 1) ? storage::JournalRole::COMMIT : storage::JournalRole::SIGNER};
    const uint64_t instance{uint64_t{buffer[0]} >> 1};
    const Bytes bytes{buffer.data() + 1, buffer.size() - 1};
    const auto scanned{storage::ScanRecords(bytes, role, instance)};
    if (!scanned.has_value()) return;
    std::vector<unsigned char> framed;
    uint256 prev{storage::JournalGenesisChain(role, instance)};
    uint64_t seq{0};
    for (const auto& record : scanned->records) {
        assert(record.seq == ++seq);
        const auto encoded{storage::EncodeRecord(prev, record.type, record.payload)};
        assert(encoded.has_value());
        assert(encoded->chain == record.chain);
        framed.insert(framed.end(), encoded->bytes.begin(), encoded->bytes.end());
        prev = encoded->chain;
    }
    assert(scanned->last_chain == prev);
    assert(scanned->end_offset == framed.size());
    assert(Same(framed, bytes.first(scanned->end_offset)));
    assert(std::ranges::all_of(bytes.subspan(scanned->end_offset), [](unsigned char b) { return b == 0; }));
}

//! A state snapshot (state/snapshot.h): bytes that decode re-encode exactly.
FUZZ_TARGET(fm2_snapshot, .init = InitProfiles)
{
    const auto [profile, bytes]{WithProfile(buffer)};
    RoundTrip(bytes, [&](Bytes b) { return state::DecodeSnapshot(b, profile); },
              [](const std::shared_ptr<const state::CoreState>& s) { return state::EncodeSnapshot(*s).value(); });
}

//! A fact batch of decoded facts: CheckBatch never crashes, and a batch it
//! accepts has at most max_facts facts and a range that does not run backwards.
FUZZ_TARGET(fm2_fact_batch)
{
    FuzzedDataProvider provider{buffer.data(), buffer.size()};
    chain::FactBatch batch;
    batch.from = {provider.ConsumeIntegral<uint32_t>(), provider.ConsumeIntegral<uint32_t>()};
    batch.to = {provider.ConsumeIntegral<uint32_t>(), provider.ConsumeIntegral<uint32_t>()};
    batch.anchor.height = provider.ConsumeIntegral<uint32_t>();
    const FactCursor after{provider.ConsumeBool() ? batch.from : FactCursor{provider.ConsumeIntegral<uint32_t>(), provider.ConsumeIntegral<uint32_t>()}};
    const size_t max_facts{provider.ConsumeIntegralInRange<size_t>(0, MAX_SLOT_FACTS + 1)};
    LIMITED_WHILE(provider.remaining_bytes() > 0, MAX_SLOT_FACTS + 2)
    {
        const auto chunk{provider.ConsumeBytes<unsigned char>(provider.ConsumeIntegralInRange<size_t>(0, 256))};
        const auto fact{DecodeFact(chunk)};
        if (fact.has_value()) batch.facts.push_back(*fact);
    }
    if (chain::CheckBatch(batch, after, max_facts).has_value()) {
        assert(batch.from == after);
        assert(batch.from <= batch.to);
        assert(batch.facts.size() <= max_facts && batch.facts.size() <= MAX_SLOT_FACTS);
    }
}

//! The seat mesh HELLO (net/tcp_mesh_wire.h): an accepted HELLO re-encodes
//! exactly, names this seat and a different seat of the set.
FUZZ_TARGET(fm2_mesh_hello)
{
    if (buffer.size() < net::HELLO_BYTES + 3) return;
    net::HelloExpectation expect;
    expect.seat_count = 1 + buffer[0] % 8;
    expect.self_seat = buffer[1] % (expect.seat_count + 1);
    expect.initiator = buffer[2] & 1;
    expect.peer = (buffer[2] >> 1) % 8;
    expect.lane = static_cast<net::Lane>(1 + (buffer[2] >> 4) % 4);
    const std::span<const unsigned char, net::HELLO_BYTES> hello_bytes{buffer.data() + 3, net::HELLO_BYTES};
    const auto hello{net::CheckHello(hello_bytes, expect)};
    if (!hello) return;
    const auto encoded{net::EncodeHello(*hello)};
    assert(std::ranges::equal(encoded, hello_bytes));
    assert(hello->to_seat == expect.self_seat && hello->from_seat != expect.self_seat && hello->from_seat < expect.seat_count);
    assert(expect.initiator ? (hello->from_seat == expect.peer && hello->lane == expect.lane) : hello->from_seat < expect.self_seat);
}
