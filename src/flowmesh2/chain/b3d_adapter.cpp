// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/chain/b3d_adapter.h>

#include <flowmesh2/core/body.h>
#include <util/chaintype.h>

#include <algorithm>
#include <limits>
#include <utility>
#include <variant>

namespace flowmesh2::chain {

namespace {
Error Invalid(std::string_view detail) { return Error{ErrorCode::INVALID_INPUT, detail}; }
} // namespace

Result<std::unique_ptr<ChainAdapter>> B3dTrustedFactsAdapter::Create(const NetworkId& network, const B3dAdapterConfig& /*config*/)
{
    // Trusted facts are a regtest-only mode (plan 3.3, Security M4).
    if (network.chain != ChainType::REGTEST) return Error{ErrorCode::UNSUPPORTED_NETWORK, "trusted facts are REGTEST only"};
    return NotImplemented("MB1-ADP-06"); // TODO(rebirth:MB1-ADP-06)
}

Result<std::unique_ptr<B3dFinalizedAdapter>> B3dFinalizedAdapter::Create(const NetworkId& network, const B3dAdapterConfig& config,
                                                                         const AuthorityVersion& authority, const SeatSet& seats,
                                                                         const Anchor& base)
{
    if (seats.Root() != authority.seat_set_root) return Invalid(B3D_SEAT_SET);
    if (base.hash.IsNull()) return Invalid(B3D_BLOCK_HASH);
    if (base.height > std::numeric_limits<uint32_t>::max() - 2) return Invalid(B3D_BASE_HEIGHT);
    if (network.chain != ChainType::REGTEST && config.min_depth < FINALIZED_MIN_DEPTH) return Invalid(B3D_MIN_DEPTH);
    return Ok(std::unique_ptr<B3dFinalizedAdapter>{new B3dFinalizedAdapter{network, config.min_depth, authority, seats, base}});
}

B3dFinalizedAdapter::B3dFinalizedAdapter(const NetworkId& network, uint32_t min_depth, const AuthorityVersion& authority,
                                         const SeatSet& seats, const Anchor& base)
    : m_network{network}, m_min_depth{min_depth}, m_base{base}, m_genesis_seats{seats}, m_authority{authority},
      m_finalized{base}, m_exposed{base}
{
    m_seat_sets.emplace(authority.epoch, seats);
}

B3dFinalizedAdapter::~B3dFinalizedAdapter() = default;

// ---- Locked helpers ----

uint32_t B3dFinalizedAdapter::TipHeightLocked() const
{
    return m_base.height + static_cast<uint32_t>(m_blocks.size());
}

Anchor B3dFinalizedAdapter::AnchorAtLocked(uint32_t height) const
{
    if (height == m_base.height) return m_base;
    return Anchor{height, m_blocks[height - m_base.height - 1].hash};
}

bool B3dFinalizedAdapter::OnChainLocked(const Anchor& anchor) const
{
    if (anchor.height < m_base.height || anchor.height > TipHeightLocked()) return false;
    return AnchorAtLocked(anchor.height).hash == anchor.hash;
}

uint32_t B3dFinalizedAdapter::UsableHeightLocked() const
{
    const uint32_t tip{TipHeightLocked()};
    const uint32_t deep{tip >= m_min_depth ? tip - m_min_depth : 0};
    return std::max(m_base.height, std::min(m_finalized.height, deep));
}

void B3dFinalizedAdapter::BumpLocked(const SeatSet& seats)
{
    ++m_authority.epoch;
    m_authority.seat_set_root = seats.Root();
    m_seat_sets.insert_or_assign(m_authority.epoch, seats);
}

void B3dFinalizedAdapter::AdvanceLocked()
{
    const uint32_t usable{UsableHeightLocked()};
    while (m_exposed.height < usable) {
        const uint32_t next{m_exposed.height + 1};
        const Block& block{m_blocks[next - m_base.height - 1]};
        if (block.seats.has_value()) BumpLocked(*block.seats); // PLAN-11 (a): the seat-set block is final now
        m_exposed = Anchor{next, block.hash};
    }
}

const SeatSet& B3dFinalizedAdapter::SeatsInForceLocked() const
{
    for (uint32_t height{m_exposed.height}; height > m_base.height; --height) {
        const Block& block{m_blocks[height - m_base.height - 1]};
        if (block.seats.has_value()) return *block.seats;
    }
    return m_genesis_seats;
}

Result<void> B3dFinalizedAdapter::VerifyAnchorLocked(const Anchor& candidate, const Anchor& last_certified) const
{
    if (candidate.height < last_certified.height) return Invalid(ANCHOR_BELOW_CERTIFIED);
    if (candidate.height > m_exposed.height) return Error{ErrorCode::NEED_DATA, ANCHOR_UNKNOWN};
    if (!OnChainLocked(candidate)) return Error{ErrorCode::VERIFY_FAILED, ANCHOR_NOT_ON_CHAIN};
    if (!OnChainLocked(last_certified)) return Error{ErrorCode::VERIFY_FAILED, ANCHOR_NOT_DESCENDANT};
    return Ok();
}

// ---- Inspection ----

AuthorityVersion B3dFinalizedAdapter::CurrentAuthority() const
{
    LOCK(m_mutex);
    return m_authority;
}

Anchor B3dFinalizedAdapter::Tip() const
{
    LOCK(m_mutex);
    return AnchorAtLocked(TipHeightLocked());
}

Anchor B3dFinalizedAdapter::Finalized() const
{
    LOCK(m_mutex);
    return m_finalized;
}

Anchor B3dFinalizedAdapter::Exposed() const
{
    LOCK(m_mutex);
    return m_exposed;
}

FactCursor B3dFinalizedAdapter::Start() const { return FactCursor{m_base.height + 1, 0}; }

FactCursor B3dFinalizedAdapter::End() const
{
    LOCK(m_mutex);
    return FactCursor{m_exposed.height + 1, 0};
}

// ---- The node link ----

Result<void> B3dFinalizedAdapter::OnConnected(ChainBlock block)
{
    if (block.block.hash.IsNull()) return Invalid(B3D_BLOCK_HASH);
    LOCK(m_mutex);
    const uint32_t tip{TipHeightLocked()};
    // The next block and the cursor after it must both exist.
    if (tip >= std::numeric_limits<uint32_t>::max() - 1 || block.block.height != tip + 1) return Invalid(B3D_HEIGHT);
    if (block.parent != AnchorAtLocked(tip).hash) return Invalid(B3D_PARENT);
    const uint32_t height{block.block.height};
    if (block.facts.size() > std::numeric_limits<uint32_t>::max() - 1) return Invalid(B3D_FACT_CURSOR);
    for (size_t i{0}; i < block.facts.size(); ++i) {
        if (!(CursorOf(block.facts[i]) == FactCursor{height, static_cast<uint32_t>(i)})) return Invalid(B3D_FACT_CURSOR);
    }
    // The fact rules of CheckBatch over the block in slot-sized pieces (a
    // chain block may hold more facts than a slot; Poll cuts batches by
    // max_facts, so the block is never refused for its size); then outpoints
    // across the block and the whole current chain.
    for (size_t begin{0}; begin < block.facts.size() || begin == 0; begin += MAX_SLOT_FACTS) {
        const size_t end{std::min(block.facts.size(), begin + MAX_SLOT_FACTS)};
        const FactCursor from{height, static_cast<uint32_t>(begin)};
        const FactCursor to{end == block.facts.size() ? FactCursor{height + 1, 0} : FactCursor{height, static_cast<uint32_t>(end)}};
        const FactBatch piece{from, to, block.block, m_authority,
                              std::vector<Fact>(block.facts.begin() + static_cast<std::ptrdiff_t>(begin), block.facts.begin() + static_cast<std::ptrdiff_t>(end))};
        if (Result<void> ok{CheckBatch(piece, from, MAX_SLOT_FACTS)}; !ok.has_value()) return ok.error();
        if (end == block.facts.size()) break;
    }
    std::set<Outpoint> added;
    for (const Fact& fact : block.facts) {
        if (const auto* deposit{std::get_if<DepositFact>(&fact)}) {
            if (m_outpoints.contains(deposit->outpoint)) return Invalid(B3D_OUTPOINT_REPEATED);
            if (!added.insert(deposit->outpoint).second) return Invalid(BATCH_DUPLICATE_OUTPOINT);
        }
    }
    m_outpoints.insert(added.begin(), added.end());
    m_blocks.push_back(Block{block.block.hash, block.parent, std::move(block.facts), std::move(block.seats)});
    AdvanceLocked(); // a deeper chain may make a block usable
    return Ok();
}

Result<void> B3dFinalizedAdapter::OnDisconnected(const Anchor& block)
{
    LOCK(m_mutex);
    const uint32_t tip{TipHeightLocked()};
    if (tip == m_base.height) return Invalid(B3D_BELOW_BASE);
    if (!(block == AnchorAtLocked(tip))) return Invalid(B3D_NOT_TIP);
    for (const Fact& fact : m_blocks.back().facts) {
        if (const auto* deposit{std::get_if<DepositFact>(&fact)}) m_outpoints.erase(deposit->outpoint);
    }
    m_blocks.pop_back();
    const Anchor new_tip{AnchorAtLocked(tip - 1)};
    if (m_finalized.height >= tip) m_finalized = new_tip; // the node's finality did not hold; follow the chain
    if (m_exposed.height >= tip) {
        // PLAN-11 (b): a reorg at or below the anchor a consumer may hold. The
        // exposure steps down to the block below, which was exposed and is
        // still on the chain, never further: a later disconnect of another
        // exposed block comes through here again and bumps again, so the
        // rewind cursor always reaches the lowest block whose facts a
        // consumer may have applied (review of 2026-11-17).
        m_exposed = new_tip;
        BumpLocked(SeatsInForceLocked());
        const FactCursor rewind_to{tip, 0};
        if (!m_rewind_to.has_value() || rewind_to < *m_rewind_to) m_rewind_to = rewind_to;
    }
    return Ok();
}

Result<void> B3dFinalizedAdapter::OnFinalized(const Anchor& block)
{
    LOCK(m_mutex);
    if (!OnChainLocked(block)) return Invalid(B3D_FINALITY_OFF_CHAIN);
    if (block.height < m_finalized.height) return Invalid(B3D_FINALITY_BACKWARDS);
    m_finalized = block;
    AdvanceLocked();
    return Ok();
}

// ---- ChainAdapter ----

Result<PollReply> B3dFinalizedAdapter::Poll(const FactCursor& after, size_t max_facts)
{
    if (max_facts == 0 || max_facts > MAX_SLOT_FACTS) return Invalid(POLL_MAX_FACTS);
    LOCK(m_mutex);
    if (m_rewind_to.has_value()) {
        if (*m_rewind_to < after) return Ok(PollReply::AuthorityChanged(m_authority, *m_rewind_to));
        m_rewind_to.reset(); // the consumer resumed at or before the rewind point
    }
    if (after < Start()) return Invalid(B3D_CURSOR_BEFORE_BASE);
    const FactCursor end{m_exposed.height + 1, 0};
    if (end < after) return Ok(PollReply::NeedData(m_authority));

    FactBatch batch;
    batch.from = after;
    batch.to = end;
    batch.anchor = m_exposed;
    batch.authority = m_authority;
    bool cut{false};
    for (uint32_t height{std::max(after.height, m_base.height + 1)}; height <= m_exposed.height && !cut; ++height) {
        for (const Fact& fact : m_blocks[height - m_base.height - 1].facts) {
            const FactCursor cursor{CursorOf(fact)};
            if (cursor < after) continue;
            if (batch.facts.size() == max_facts) {
                cut = true;
                break;
            }
            batch.facts.push_back(fact);
        }
    }
    if (cut) {
        const FactCursor last{CursorOf(batch.facts.back())};
        batch.to = FactCursor{last.height, last.index + 1};
    }
    return Ok(PollReply::Facts(std::move(batch)));
}

Result<void> B3dFinalizedAdapter::VerifyAnchor(const Anchor& candidate, const Anchor& last_certified) const
{
    LOCK(m_mutex);
    return VerifyAnchorLocked(candidate, last_certified);
}

Result<AnchorProof> B3dFinalizedAdapter::ProveAnchor(const Anchor& candidate, const Anchor& from) const
{
    LOCK(m_mutex);
    if (Result<void> ok{VerifyAnchorLocked(candidate, from)}; !ok.has_value()) return ok.error();
    AnchorProof proof{from, candidate, {}, m_finalized};
    for (uint32_t height{from.height + 1}; height <= candidate.height; ++height) {
        const Block& block{m_blocks[height - m_base.height - 1]};
        proof.path.push_back(HeaderLink{height, block.hash, block.parent});
    }
    return Ok(std::move(proof));
}

Result<SeatSet> B3dFinalizedAdapter::SeatSetAt(uint64_t epoch) const
{
    LOCK(m_mutex);
    const auto it{m_seat_sets.find(epoch)};
    if (it == m_seat_sets.end()) return Error{ErrorCode::NEED_DATA, SEAT_SET_UNKNOWN_EPOCH};
    return Ok(it->second);
}

} // namespace flowmesh2::chain
