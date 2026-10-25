// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license.
#ifndef B3COIN_BRIDGE_GLOAS_PEER_LIGHT_CLIENT_H
#define B3COIN_BRIDGE_GLOAS_PEER_LIGHT_CLIENT_H

#include <bridge/peer_light_client.h>
#include <bridge/rlp.h>
#include <crypto/keccak256.h>

#include <variant>

/** Separate Sepolia/Gloas peer authority. V1 mainnet records, clocks, policy
 * digests and journals are deliberately not interpreted by this module.
 * Gloas authenticates an EL block HASH, not an SSZ ExecutionPayloadHeader.
 * State/receipt roots enter a native capability only after checking the exact
 * Amsterdam RLP header preimage against that authenticated hash.
 */
namespace bridge::peer::gloas {
using Bytes = std::vector<unsigned char>;
inline constexpr uint64_t CHAIN_ID{11'155'111};
inline constexpr uint64_t GENESIS_TIME{1'655'733'600};
inline constexpr uint64_t FORK_EPOCH{353'024};
inline constexpr uint64_t REVIEWED_THROUGH_EPOCH{365'000};
inline constexpr uint64_t FINALITY_GINDEX{735}, CURRENT_GINDEX{2945}, NEXT_GINDEX{2946}, EXECUTION_GINDEX{2856};
inline constexpr std::array<unsigned char, 4> FORK_VERSION{0x90,0,0,0x76};
inline constexpr std::array<unsigned char,32> GENESIS_VALIDATORS_ROOT{
    0xd8,0xea,0x17,0x1f,0x3c,0x94,0xae,0xa2,0x1e,0xbc,0x42,0xa1,0xed,0x61,0x05,0x2a,
    0xcf,0x3f,0x92,0x09,0xc0,0x0e,0x4e,0xfb,0xaa,0xdd,0xac,0x09,0xed,0x9b,0x80,0x78};
inline constexpr std::array<unsigned char,8> RECORD_MAGIC{'B','3','G','L','P','R','0','1'};
inline constexpr std::array<unsigned char,8> JOURNAL_MAGIC{'B','3','G','L','J','R','0','1'};
inline constexpr size_t MAX_RECORD_BYTES{65'536}, MAX_EL_HEADER_BYTES{4096};

struct Policy {
    uint256 trusted_root{};
    uint64_t trusted_slot{0}, fork_schedule_valid_through_epoch{0};
    uint64_t max_sync_lag_slots{0}, max_recovery_lag_slots{0};
    unsigned min_participants{342};
    bool KnownSlot(uint64_t slot) const {
        const auto e{EpochAtSlot(slot)};
        return e >= FORK_EPOCH && e <= fork_schedule_valid_through_epoch && e <= REVIEWED_THROUGH_EPOCH;
    }
    bool Valid() const {
        return !trusted_root.IsNull() && trusted_slot && KnownSlot(trusted_slot) &&
            fork_schedule_valid_through_epoch <= REVIEWED_THROUGH_EPOCH &&
            max_sync_lag_slots && max_sync_lag_slots <= SLOTS_PER_PERIOD &&
            max_recovery_lag_slots && max_recovery_lag_slots <= SLOTS_PER_PERIOD &&
            min_participants >= 342 && min_participants <= 512;
    }
    uint256 Digest() const {
        proof_detail::Writer w;
        w.U64(CHAIN_ID); w.Root(uint256{std::span{GENESIS_VALIDATORS_ROOT}}); w.U64(GENESIS_TIME);
        w.U64(12); w.U64(FORK_EPOCH); w.Bytes(FORK_VERSION);
        w.U64(FINALITY_GINDEX); w.U64(CURRENT_GINDEX); w.U64(NEXT_GINDEX); w.U64(EXECUTION_GINDEX);
        w.Root(trusted_root); w.U64(trusted_slot); w.U64(fork_schedule_valid_through_epoch);
        w.U64(max_sync_lag_slots); w.U64(max_recovery_lag_slots); w.U32(min_participants);
        return TaggedDigest("B3/ethereum-sepolia-gloas-policy/1", w.Take());
    }
};
struct Header {
    ssz::BeaconBlockHeader beacon{};
    uint256 execution_block_hash{};
    std::vector<uint256> execution_branch;
    bool Verify() const {
        return EpochAtSlot(beacon.slot) >= FORK_EPOCH && !execution_block_hash.IsNull() &&
            ssz::VerifyBranch(execution_block_hash, execution_branch, EXECUTION_GINDEX, beacon.body_root);
    }
    friend bool operator==(const Header&, const Header&) = default;
};
struct Bootstrap { Header header; ssz::SyncCommittee current; std::vector<uint256> branch; };
struct Update {
    Header attested, finalized;
    std::vector<uint256> finality_branch;
    bool has_next{false};
    ssz::SyncCommittee next;
    std::vector<uint256> next_branch;
    SyncAggregate sync;
    uint64_t signature_slot{0};
};
using Record = std::variant<Bootstrap, Update>;
struct Store {
    Header finalized;
    uint64_t period{0};
    ssz::SyncCommittee current;
    std::optional<ssz::SyncCommittee> next;
    friend bool operator==(const Store&, const Store&) = default;
};

namespace detail {
inline std::optional<uint64_t> Uint64(const RlpItem& item) {
    if (item.is_list || item.payload.size()>8 || (!item.payload.empty() && item.payload[0]==0)) return std::nullopt;
    uint64_t out{0}; for (const auto b : item.payload) out=(out<<8)|b; return out;
}
inline bool HeaderOut(proof_detail::Writer& w, const Header& h) {
    if (h.execution_branch.size() != 11) return false;
    w.U64(h.beacon.slot); w.U64(h.beacon.proposer_index); w.Root(h.beacon.parent_root);
    w.Root(h.beacon.state_root); w.Root(h.beacon.body_root); w.Root(h.execution_block_hash);
    for (const auto& x : h.execution_branch) w.Root(x);
    return true;
}
inline bool HeaderIn(proof_detail::Reader& r, Header& h) {
    if (!r.U64(h.beacon.slot) || !r.U64(h.beacon.proposer_index) || !r.Root(h.beacon.parent_root) ||
        !r.Root(h.beacon.state_root) || !r.Root(h.beacon.body_root) || !r.Root(h.execution_block_hash) || r.Remaining() < 352) return false;
    h.execution_branch.resize(11);
    for (auto& x : h.execution_branch) if (!r.Root(x)) return false;
    return true;
}
inline bool BranchOut(proof_detail::Writer& w, const std::vector<uint256>& b, size_t expected) {
    if (b.size()!=expected || (expected!=9 && expected!=11)) return false;
    w.U8(static_cast<uint8_t>(expected)); for (const auto& node:b) w.Root(node); return true;
}
inline bool BranchIn(proof_detail::Reader& r, std::vector<uint256>& b, size_t expected) {
    uint8_t count{};
    if ((expected!=9 && expected!=11) || !r.U8(count) || count!=expected || r.Remaining()<expected*32) return false;
    b.resize(expected); for (auto& node:b) if (!r.Root(node)) return false; return true;
}
} // namespace detail

inline std::optional<Bytes> EncodeRecord(const Record& record) {
    proof_detail::Writer w; w.Bytes(RECORD_MAGIC);
    if (const auto* b{std::get_if<Bootstrap>(&record)}) {
        w.U8(1);
        if (!detail::HeaderOut(w,b->header) || !proof_detail::EncodeCommittee(w,b->current) ||
            !detail::BranchOut(w,b->branch,11)) return std::nullopt;
    } else {
        const auto& u{std::get<Update>(record)};
        w.U8(2);
        if (!detail::HeaderOut(w,u.attested) || !detail::HeaderOut(w,u.finalized) ||
            !detail::BranchOut(w,u.finality_branch,9)) return std::nullopt;
        w.U8(u.has_next ? 1 : 0);
        if (u.has_next) {
            if (!proof_detail::EncodeCommittee(w,u.next) || !detail::BranchOut(w,u.next_branch,11)) return std::nullopt;
        } else if (!proof_detail::AbsentCommitteeCanonical(u.next) || !u.next_branch.empty()) return std::nullopt;
        w.Bytes(u.sync.bits); w.Bytes(u.sync.signature); w.U64(u.signature_slot);
    }
    auto out{w.Take()};
    return out.size() <= MAX_RECORD_BYTES ? std::optional<Bytes>{std::move(out)} : std::nullopt;
}
inline std::optional<Record> DecodeRecord(std::span<const unsigned char> bytes) {
    if (bytes.size() > MAX_RECORD_BYTES) return std::nullopt;
    proof_detail::Reader r{bytes}; std::array<unsigned char,8> magic{}; uint8_t kind{};
    if (!r.Bytes(magic) || magic != RECORD_MAGIC || !r.U8(kind)) return std::nullopt;
    if (kind == 1) {
        Bootstrap b;
        if (!detail::HeaderIn(r,b.header) || !proof_detail::DecodeCommittee(r,b.current) ||
            !detail::BranchIn(r,b.branch,11) || !r.Empty()) return std::nullopt;
        return Record{std::move(b)};
    }
    if (kind != 2) return std::nullopt;
    Update u; uint8_t next{};
    if (!detail::HeaderIn(r,u.attested) || !detail::HeaderIn(r,u.finalized) ||
        !detail::BranchIn(r,u.finality_branch,9) || !r.U8(next) || next > 1) return std::nullopt;
    u.has_next = next;
    if (u.has_next && (!proof_detail::DecodeCommittee(r,u.next) || !detail::BranchIn(r,u.next_branch,11))) return std::nullopt;
    if (!r.Bytes(u.sync.bits) || !r.Bytes(u.sync.signature) || !r.U64(u.signature_slot) || !r.Empty()) return std::nullopt;
    return Record{std::move(u)};
}

struct ExecutionHeader {
    uint256 block_hash{}, parent_hash{}, state_root{}, receipts_root{};
    uint64_t block_number{0}, timestamp{0}, slot_number{0};
    friend bool operator==(const ExecutionHeader&, const ExecutionHeader&) = default;
};
/** Geth Amsterdam header: post-Prague21 + EIP7928 BAL hash + EIP7843 slot.
 * The exact RLP bytes are the hash preimage; a future extension is refused.
 */
inline std::optional<ExecutionHeader> DecodeExecutionHeader(std::span<const unsigned char> bytes) {
    if (bytes.empty() || bytes.size() > MAX_EL_HEADER_BYTES) return std::nullopt;
    const auto top{RlpDecode(bytes)};
    const auto f{top && top->is_list ? RlpChildren(*top) : std::nullopt};
    if (!f || f->size() != 23) return std::nullopt;
    const auto fixed{[&](size_t i,size_t n) { return !(*f)[i].is_list && (*f)[i].payload.size() == n; }};
    const auto integer{[&](size_t i,size_t n) { const auto& v{(*f)[i]}; return !v.is_list && v.payload.size() <= n && (v.payload.empty() || v.payload[0] != 0); }};
    for (size_t i : {0U,1U,3U,4U,5U,13U,16U,19U,20U,21U}) if (!fixed(i,32)) return std::nullopt;
    if (!fixed(2,20) || !fixed(6,256) || !fixed(14,8) || (*f)[12].is_list || (*f)[12].payload.size() > 32 ||
        !integer(7,32) || !integer(15,32)) return std::nullopt;
    for (size_t i : {8U,9U,10U,11U,17U,18U,22U}) if (!integer(i,8)) return std::nullopt;
    const auto number{detail::Uint64((*f)[8])}, time{detail::Uint64((*f)[11])};
    const auto gas{detail::Uint64((*f)[10])}, limit{detail::Uint64((*f)[9])};
    const auto slot{detail::Uint64((*f)[22])};
    if (!number || !*number || !time || !*time || !gas || !limit || *gas > *limit || !slot) return std::nullopt;
    ExecutionHeader h;
    Keccak256().Write(bytes).Finalize(h.block_hash);
    h.parent_hash = uint256{(*f)[0].payload}; h.state_root = uint256{(*f)[3].payload}; h.receipts_root = uint256{(*f)[5].payload};
    h.block_number = *number; h.timestamp = *time; h.slot_number = *slot;
    if (h.state_root.IsNull() || h.receipts_root.IsNull()) return std::nullopt;
    return h;
}

class LightClient;
class FinalizedExecution final {
public:
    uint64_t ChainId() const { return CHAIN_ID; }
    uint256 GenesisValidatorsRoot() const { return uint256{std::span{GENESIS_VALIDATORS_ROOT}}; }
    const uint256& PolicyDigest() const { return m_policy; }
    const uint256& JournalDigest() const { return m_journal; }
    uint64_t Sequence() const { return m_sequence; }
    uint64_t BeaconSlot() const { return m_slot; }
    const ExecutionHeader& Header() const { return m_header; }
private:
    friend class LightClient;
    FinalizedExecution(const LightClient* owner,uint256 policy,uint256 journal,uint64_t sequence,uint64_t slot,ExecutionHeader header)
        : m_owner{owner},m_policy{policy},m_journal{journal},m_sequence{sequence},m_slot{slot},m_header{std::move(header)} {}
    const LightClient* m_owner;
    uint256 m_policy{},m_journal{};
    uint64_t m_sequence{0},m_slot{0};
    ExecutionHeader m_header;
};

class LightClient final {
public:
    using DurableAppend = std::function<bool(std::span<const unsigned char>)>;
    explicit LightClient(Policy policy) : m_policy{std::move(policy)},m_policy_digest{m_policy.Digest()},m_fenced{!m_policy.Valid()} {}
    bool Fenced() const { return m_fenced; }
    bool Replaying() const { return m_replaying; }
    uint64_t Sequence() const { return m_sequence; }
    const uint256& PolicyDigest() const { return m_policy_digest; }
    const uint256& JournalDigest() const { return m_digest; }
    const std::optional<Store>& State() const { return m_store; }
    bool EndReplay() { if (m_fenced || !m_replaying) return false; m_replaying=false; return true; }
    Reply ReplayEntry(std::span<const unsigned char> bytes) {
        if (m_fenced || !m_replaying) return {Result::STORAGE_FENCED};
        const auto bad{[&]() { m_fenced=true; return Reply{Result::BAD_JOURNAL}; }};
        if (m_sequence >= MAX_JOURNAL_ENTRIES || bytes.size() < ENTRY_OVERHEAD || bytes.size() > ENTRY_OVERHEAD+MAX_RECORD_BYTES) return bad();
        proof_detail::Reader r{bytes}; std::array<unsigned char,8> magic{}; uint64_t seq{}; uint32_t size{};
        uint256 policy,previous,digest; Bytes record;
        if (!r.Bytes(magic) || magic!=JOURNAL_MAGIC || !r.U64(seq) || seq!=m_sequence+1 || !r.Root(policy) || policy!=m_policy_digest ||
            !r.Root(previous) || previous!=m_digest || !r.U32(size) || size>MAX_RECORD_BYTES || !r.Vector(size,record) ||
            !r.Root(digest) || !r.Empty() || digest!=TaggedDigest("B3/ethereum-sepolia-gloas-journal/1",bytes.first(bytes.size()-32))) return bad();
        std::optional<Store> next;
        const auto result{Verify(record,next)};
        if (result.result!=Result::OK) return bad();
        m_store=std::move(next); m_sequence=seq; m_digest=digest; return {Result::OK,LcResult::OK};
    }
    Reply Submit(std::span<const unsigned char> record,uint64_t current_slot,const DurableAppend& append) {
        if (m_fenced || m_replaying) return {Result::STORAGE_FENCED};
        if (m_sequence>=MAX_JOURNAL_ENTRIES) return {Result::LIMIT};
        const auto authority{m_store ? m_store->finalized.beacon.slot : m_policy.trusted_slot};
        if (current_slot<authority) return {Result::FUTURE_SLOT};
        if (current_slot-authority>m_policy.max_recovery_lag_slots) return {Result::STALE_AUTHORITY};
        if (!m_policy.KnownSlot(current_slot)) return {Result::UNKNOWN_FORK};
        const auto decoded{DecodeRecord(record)};
        if (!decoded) return {Result::BAD_RECORD};
        const auto slot{std::holds_alternative<Bootstrap>(*decoded) ? std::get<Bootstrap>(*decoded).header.beacon.slot : std::get<Update>(*decoded).signature_slot};
        if (slot>current_slot) return {Result::FUTURE_SLOT};
        std::optional<Store> next; const auto checked{Verify(record,next)};
        if (checked.result!=Result::OK) return checked;
        proof_detail::Writer w; w.Bytes(JOURNAL_MAGIC); w.U64(m_sequence+1); w.Root(m_policy_digest); w.Root(m_digest);
        w.U32(static_cast<uint32_t>(record.size())); w.Bytes(record); auto entry{w.Take()};
        const auto digest{TaggedDigest("B3/ethereum-sepolia-gloas-journal/1",entry)}; entry.insert(entry.end(),digest.begin(),digest.end());
        try { if (!append(entry)) { m_fenced=true; return {Result::STORAGE_FENCED}; } }
        catch (...) { m_fenced=true; return {Result::STORAGE_FENCED}; }
        m_store=std::move(next); ++m_sequence; m_digest=digest; return {Result::OK,LcResult::OK};
    }
    std::optional<uint256> FinalizedHash(uint64_t current_slot) const {
        if (m_fenced || m_replaying || !m_store || !m_policy.KnownSlot(current_slot)) return std::nullopt;
        const auto slot{m_store->finalized.beacon.slot};
        if (slot>current_slot || current_slot-slot>m_policy.max_sync_lag_slots) return std::nullopt;
        return m_store->finalized.execution_block_hash; // availability request only, not root authority
    }
    std::optional<FinalizedExecution> Latest(uint64_t current_slot,std::span<const unsigned char> raw_header) const {
        const auto want{FinalizedHash(current_slot)};
        if (!want) return std::nullopt;
        const auto h{DecodeExecutionHeader(raw_header)};
        if (!h || h->block_hash!=*want || h->timestamp>GENESIS_TIME+m_store->finalized.beacon.slot*12 || h->slot_number>m_store->finalized.beacon.slot) return std::nullopt;
        return FinalizedExecution{this,m_policy_digest,m_digest,m_sequence,m_store->finalized.beacon.slot,*h};
    }
    bool IsCurrent(const FinalizedExecution& anchor,uint64_t current_slot) const {
        const auto want{FinalizedHash(current_slot)};
        return want && anchor.m_owner==this && anchor.PolicyDigest()==m_policy_digest && anchor.JournalDigest()==m_digest && anchor.Sequence()==m_sequence &&
            anchor.BeaconSlot()==m_store->finalized.beacon.slot && anchor.Header().block_hash==*want;
    }
private:
    Reply Verify(std::span<const unsigned char> bytes,std::optional<Store>& next) const {
        const auto record{DecodeRecord(bytes)};
        if (!record) return {Result::BAD_RECORD};
        const auto canonical{EncodeRecord(*record)};
        if (!canonical || canonical->size()!=bytes.size() || !std::equal(canonical->begin(),canonical->end(),bytes.begin())) return {Result::BAD_RECORD};
        if (const auto* b{std::get_if<Bootstrap>(&*record)}) {
            if (m_store || b->header.beacon.slot!=m_policy.trusted_slot || !m_policy.KnownSlot(b->header.beacon.slot)) return {Result::BAD_RECORD};
            if (b->header.beacon.HashTreeRoot()!=m_policy.trusted_root || !b->header.Verify() ||
                b->current.pubkeys.size()!=ssz::SYNC_COMMITTEE_SIZE ||
                !ssz::VerifyBranch(b->current.HashTreeRoot(),b->branch,CURRENT_GINDEX,b->header.beacon.state_root)) return {Result::BAD_PROOF,LcResult::BOOTSTRAP_PROOF};
            next=Store{b->header,PeriodAtSlot(b->header.beacon.slot),b->current,{}};
        } else {
            if (!m_store) return {Result::BOOTSTRAP_REQUIRED};
            const auto& u{std::get<Update>(*record)};
            if (!m_policy.KnownSlot(u.attested.beacon.slot) || !m_policy.KnownSlot(u.finalized.beacon.slot) || !m_policy.KnownSlot(u.signature_slot)) return {Result::UNKNOWN_FORK};
            next=*m_store; const auto proof{Apply(*next,u)};
            if (proof!=LcResult::OK) return {Result::BAD_PROOF,proof};
            if (*next==*m_store) return {Result::NO_CHANGE,LcResult::OK};
        }
        return {Result::OK,LcResult::OK};
    }
    LcResult Apply(Store& store,const Update& u) const {
        if (u.has_next && u.next.pubkeys.size()!=ssz::SYNC_COMMITTEE_SIZE) return LcResult::BAD_STRUCTURE;
        if (!(u.signature_slot>u.attested.beacon.slot && u.attested.beacon.slot>=u.finalized.beacon.slot)) return LcResult::MONOTONICITY;
        const auto ap{PeriodAtSlot(u.attested.beacon.slot)},fp{PeriodAtSlot(u.finalized.beacon.slot)},sp{PeriodAtSlot(u.signature_slot)};
        const bool fills{u.has_next && !store.next && ap==store.period && fp==store.period && sp==store.period};
        if (u.finalized.beacon.slot<store.finalized.beacon.slot && !fills) return LcResult::MONOTONICITY;
        if (u.has_next && store.next && ap==store.period && u.next.HashTreeRoot()!=store.next->HashTreeRoot()) return LcResult::NEXT_PROOF;
        if (!ssz::VerifyBranch(u.finalized.beacon.HashTreeRoot(),u.finality_branch,FINALITY_GINDEX,u.attested.beacon.state_root)) return LcResult::FINALITY_PROOF;
        if (!u.attested.Verify() || !u.finalized.Verify()) return LcResult::EXECUTION_PROOF;
        if (u.finalized.beacon.slot==store.finalized.beacon.slot && u.finalized.beacon.HashTreeRoot()!=store.finalized.beacon.HashTreeRoot()) return LcResult::MONOTONICITY;
        if (u.has_next && (ap!=sp || !ssz::VerifyBranch(u.next.HashTreeRoot(),u.next_branch,NEXT_GINDEX,u.attested.beacon.state_root))) return LcResult::NEXT_PROOF;
        const ssz::SyncCommittee* committee{sp==store.period ? &store.current : sp==store.period+1 && store.next ? &*store.next : nullptr};
        if (!committee) return LcResult::PERIOD;
        const auto count{u.sync.Participation()};
        if (count<m_policy.min_participants) return LcResult::PARTICIPATION;
        const auto domain{ssz::SyncCommitteeDomain(FORK_VERSION,uint256{std::span{GENESIS_VALIDATORS_ROOT}})};
        const auto signing_root{ssz::SigningRoot(u.attested.beacon.HashTreeRoot(),domain)};
        std::vector<bls::VerifiedPublicKey> keys; keys.reserve(count);
        for (size_t i{0};i<ssz::SYNC_COMMITTEE_SIZE;++i) if (u.sync.Participant(i)) {
            const auto pk{bls::PublicKey::Decode(committee->pubkeys[i])};
            if (!pk) return LcResult::BAD_STRUCTURE;
            keys.push_back(bls::VerifiedPublicKey::TrustedFromValidatedChain(*pk));
        }
        const auto sig{bls::Signature::Decode(u.sync.signature)};
        if (!sig) return LcResult::BAD_STRUCTURE;
        if (!bls::FastAggregateVerify(keys,std::span<const unsigned char>{signing_root.begin(),32},*sig)) return LcResult::SIGNATURE;
        if (fp==store.period+1) {
            if (!store.next) return LcResult::PERIOD;
            store.current=*store.next; store.next.reset(); store.period=fp;
        } else if (fp!=store.period) return LcResult::PERIOD;
        if (u.finalized.beacon.slot>=store.finalized.beacon.slot) store.finalized=u.finalized;
        if (u.has_next && !store.next && ap==store.period) store.next=u.next;
        return LcResult::OK;
    }
    Policy m_policy;
    uint256 m_policy_digest{},m_digest{};
    std::optional<Store> m_store;
    uint64_t m_sequence{0};
    bool m_replaying{true},m_fenced{false};
};
} // namespace bridge::peer::gloas
#endif
