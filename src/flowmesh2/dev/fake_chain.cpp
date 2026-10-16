// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/dev/fake_chain.h>

#include <flowmesh2/core/body.h>
#include <util/chaintype.h>

#include <limits>
#include <utility>
#include <variant>

namespace flowmesh2::dev {

std::string_view ToString(HookPoint point)
{
    switch (point) {
    case HookPoint::POLL: return "POLL";
    case HookPoint::SIGNER_BARRIER_START: return "SIGNER_BARRIER_START";
    case HookPoint::SIGNER_BARRIER_END: return "SIGNER_BARRIER_END";
    case HookPoint::COMMIT_BARRIER_START: return "COMMIT_BARRIER_START";
    case HookPoint::COMMIT_BARRIER_END: return "COMMIT_BARRIER_END";
    }
    return "UNKNOWN";
}

namespace {
Error Invalid(std::string_view detail) { return Error{ErrorCode::INVALID_INPUT, detail}; }
} // namespace

Result<std::unique_ptr<FakeChainAdapter>> FakeChainAdapter::Create(const NetworkId& network, const AuthorityVersion& authority,
                                                                   const SeatSet& seats, const Anchor& base)
{
    if (network.chain != ChainType::REGTEST) return Error{ErrorCode::UNSUPPORTED_NETWORK, FAKE_NETWORK};
    if (seats.Root() != authority.seat_set_root) return Invalid(FAKE_SEAT_SET);
    if (base.hash.IsNull()) return Invalid(FAKE_BLOCK_HASH);
    if (base.height > std::numeric_limits<uint32_t>::max() - 2) return Invalid(FAKE_BASE_HEIGHT);
    return Ok(std::unique_ptr<FakeChainAdapter>{new FakeChainAdapter{network, authority, seats, base}});
}

FakeChainAdapter::FakeChainAdapter(const NetworkId& network, const AuthorityVersion& authority, const SeatSet& seats,
                                   const Anchor& base)
    : m_network{network}, m_base{base}, m_journal_hook{*this}, m_authority{authority}
{
    m_seat_sets.emplace(authority.epoch, seats);
}

FakeChainAdapter::~FakeChainAdapter() = default;

AuthorityVersion FakeChainAdapter::CurrentAuthority() const
{
    LOCK(m_mutex);
    return m_authority;
}

uint32_t FakeChainAdapter::TipHeightLocked() const
{
    return m_base.height + static_cast<uint32_t>(m_blocks.size());
}

Anchor FakeChainAdapter::Tip() const
{
    LOCK(m_mutex);
    if (m_blocks.empty()) return m_base;
    return Anchor{TipHeightLocked(), m_blocks.back().hash};
}

FactCursor FakeChainAdapter::Start() const { return FactCursor{m_base.height + 1, 0}; }

FactCursor FakeChainAdapter::End() const
{
    LOCK(m_mutex);
    return FactCursor{TipHeightLocked() + 1, 0};
}

uint64_t FakeChainAdapter::Reached(HookPoint point) const
{
    LOCK(m_mutex);
    const auto it{m_reached.find(point)};
    return it == m_reached.end() ? 0 : it->second;
}

AuthorityVersion FakeChainAdapter::BumpLocked(const std::optional<SeatSet>& seats)
{
    ++m_authority.epoch;
    if (seats.has_value()) m_authority.seat_set_root = seats->Root();
    const SeatSet& current{seats.has_value() ? *seats : m_seat_sets.rbegin()->second};
    m_seat_sets.insert_or_assign(m_authority.epoch, current);
    return m_authority;
}

AuthorityVersion FakeChainAdapter::BumpAuthority(const std::optional<SeatSet>& seats)
{
    LOCK(m_mutex);
    return BumpLocked(seats);
}

void FakeChainAdapter::QueueBusy(uint32_t polls)
{
    LOCK(m_mutex);
    m_busy += polls;
}

Result<void> FakeChainAdapter::BumpAt(HookPoint point, uint64_t occurrence, const std::optional<SeatSet>& seats)
{
    if (occurrence == 0) return Invalid(FAKE_HOOK_OCCURRENCE);
    LOCK(m_mutex);
    m_hooks.push_back(Hook{point, occurrence, seats});
    return Ok();
}

void FakeChainAdapter::Reach(HookPoint point)
{
    LOCK(m_mutex);
    ++m_reached[point];
    // Every hook armed for this point counts down; those that reach zero fire, in arming order.
    std::vector<Hook> kept;
    for (Hook& hook : m_hooks) {
        if (hook.point == point && --hook.remaining == 0) {
            BumpLocked(hook.seats);
        } else {
            kept.push_back(std::move(hook));
        }
    }
    m_hooks = std::move(kept);
}

void FakeChainAdapter::JournalHookObserver::OnBarrierStart(storage::JournalRole role, uint64_t, uint64_t)
{
    m_fake.Reach(role == storage::JournalRole::SIGNER ? HookPoint::SIGNER_BARRIER_START : HookPoint::COMMIT_BARRIER_START);
}

void FakeChainAdapter::JournalHookObserver::OnBarrierEnd(storage::JournalRole role, uint64_t, uint64_t, bool)
{
    m_fake.Reach(role == storage::JournalRole::SIGNER ? HookPoint::SIGNER_BARRIER_END : HookPoint::COMMIT_BARRIER_END);
}

Result<Anchor> FakeChainAdapter::AppendBlock(const uint256& hash, std::vector<Fact> facts)
{
    if (hash.IsNull()) return Invalid(FAKE_BLOCK_HASH);
    LOCK(m_mutex);
    const uint32_t tip{TipHeightLocked()};
    // The next block and the cursor after it must both exist.
    if (tip >= std::numeric_limits<uint32_t>::max() - 1) return Invalid(FAKE_CHAIN_FULL);
    const uint32_t height{tip + 1};
    if (facts.size() > MAX_SLOT_FACTS) return Invalid(chain::BATCH_TOO_MANY);
    for (size_t i{0}; i < facts.size(); ++i) {
        if (!(CursorOf(facts[i]) == FactCursor{height, static_cast<uint32_t>(i)})) return Invalid(FAKE_FACT_CURSOR);
    }
    // The fact rules of CheckBatch, over this block as one batch; then
    // outpoints across the whole current chain.
    chain::FactBatch block_batch{FactCursor{height, 0}, FactCursor{height + 1, 0}, Anchor{height, hash}, m_authority, facts};
    if (Result<void> ok{chain::CheckBatch(block_batch, block_batch.from, MAX_SLOT_FACTS)}; !ok.has_value()) {
        return ok.error();
    }
    std::vector<Outpoint> added;
    for (const Fact& fact : facts) {
        if (const auto* deposit{std::get_if<DepositFact>(&fact)}) {
            if (m_outpoints.contains(deposit->outpoint)) return Invalid(FAKE_OUTPOINT_REPEATED);
            added.push_back(deposit->outpoint);
        }
    }
    m_outpoints.insert(added.begin(), added.end());
    m_blocks.push_back(Block{hash, std::move(facts)});
    return Ok(Anchor{height, hash});
}

Result<AuthorityVersion> FakeChainAdapter::Rewind(uint32_t fork_height)
{
    LOCK(m_mutex);
    if (fork_height < m_base.height || fork_height >= TipHeightLocked()) return Invalid(FAKE_REWIND);
    const size_t keep{static_cast<size_t>(fork_height - m_base.height)};
    for (size_t i{keep}; i < m_blocks.size(); ++i) {
        for (const Fact& fact : m_blocks[i].facts) {
            if (const auto* deposit{std::get_if<DepositFact>(&fact)}) m_outpoints.erase(deposit->outpoint);
        }
    }
    m_blocks.resize(keep);
    const FactCursor rewind_to{fork_height + 1, 0};
    if (!m_rewind_to.has_value() || rewind_to < *m_rewind_to) m_rewind_to = rewind_to;
    return Ok(BumpLocked(std::nullopt));
}

Result<chain::PollReply> FakeChainAdapter::Poll(const FactCursor& after, size_t max_facts)
{
    if (max_facts == 0 || max_facts > MAX_SLOT_FACTS) return Invalid(chain::POLL_MAX_FACTS);
    Reach(HookPoint::POLL);
    LOCK(m_mutex);
    if (m_busy > 0) {
        --m_busy;
        return Ok(chain::PollReply::Busy(m_authority));
    }
    if (m_rewind_to.has_value()) {
        if (*m_rewind_to < after) return Ok(chain::PollReply::AuthorityChanged(m_authority, *m_rewind_to));
        m_rewind_to.reset(); // the consumer resumed at or before the rewind point
    }
    if (after < Start()) return Invalid(FAKE_CURSOR_BEFORE_BASE);
    const FactCursor end{TipHeightLocked() + 1, 0};
    if (end < after) return Ok(chain::PollReply::NeedData(m_authority));

    chain::FactBatch batch;
    batch.from = after;
    batch.to = end;
    batch.anchor = m_blocks.empty() ? m_base : Anchor{TipHeightLocked(), m_blocks.back().hash};
    batch.authority = m_authority;
    bool cut{false};
    for (const Block& block : m_blocks) {
        for (const Fact& fact : block.facts) {
            const FactCursor cursor{CursorOf(fact)};
            if (cursor < after) continue;
            if (batch.facts.size() == max_facts) {
                cut = true;
                break;
            }
            batch.facts.push_back(fact);
        }
        if (cut) break;
    }
    if (cut) {
        const FactCursor last{CursorOf(batch.facts.back())};
        batch.to = FactCursor{last.height, last.index + 1};
    }
    return Ok(chain::PollReply::Facts(std::move(batch)));
}

Result<void> FakeChainAdapter::VerifyAnchor(const Anchor& candidate, const Anchor& last_certified) const
{
    if (candidate.height < last_certified.height) return Invalid(chain::ANCHOR_BELOW_CERTIFIED);
    LOCK(m_mutex);
    const auto on_chain = [&](const Anchor& anchor) EXCLUSIVE_LOCKS_REQUIRED(m_mutex) {
        if (anchor.height < m_base.height || anchor.height > TipHeightLocked()) return false;
        if (anchor.height == m_base.height) return anchor.hash == m_base.hash;
        return m_blocks[anchor.height - m_base.height - 1].hash == anchor.hash;
    };
    if (candidate.height > TipHeightLocked()) return Error{ErrorCode::NEED_DATA, chain::ANCHOR_UNKNOWN};
    if (!on_chain(candidate)) return Error{ErrorCode::VERIFY_FAILED, chain::ANCHOR_NOT_ON_CHAIN};
    if (!on_chain(last_certified)) return Error{ErrorCode::VERIFY_FAILED, chain::ANCHOR_NOT_DESCENDANT};
    return Ok();
}

Result<SeatSet> FakeChainAdapter::SeatSetAt(uint64_t epoch) const
{
    LOCK(m_mutex);
    const auto it{m_seat_sets.find(epoch)};
    if (it == m_seat_sets.end()) return Error{ErrorCode::NEED_DATA, chain::SEAT_SET_UNKNOWN_EPOCH};
    return Ok(it->second);
}

} // namespace flowmesh2::dev
