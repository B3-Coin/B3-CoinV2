// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/chain/chain_adapter.h>

#include <flowmesh2/core/body.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/state_root.h>

#include <limits>
#include <set>
#include <utility>
#include <variant>

namespace flowmesh2::chain {

bool IsKnown(PollStatus status)
{
    switch (status) {
    case PollStatus::FACTS:
    case PollStatus::BUSY:
    case PollStatus::NEED_DATA:
    case PollStatus::AUTHORITY_CHANGED:
        return true;
    }
    return false;
}

std::string_view ToString(PollStatus status)
{
    switch (status) {
    case PollStatus::FACTS: return "FACTS";
    case PollStatus::BUSY: return "BUSY";
    case PollStatus::NEED_DATA: return "NEED_DATA";
    case PollStatus::AUTHORITY_CHANGED: return "AUTHORITY_CHANGED";
    }
    return "UNKNOWN";
}

PollReply::PollReply(PollStatus status, const AuthorityVersion& authority, std::optional<FactBatch> batch,
                     std::optional<FactCursor> rewind_to)
    : m_status{status}, m_authority{authority}, m_batch{std::move(batch)}, m_rewind_to{rewind_to}
{
}

PollReply PollReply::Facts(FactBatch batch)
{
    const AuthorityVersion authority{batch.authority};
    return PollReply{PollStatus::FACTS, authority, std::move(batch), std::nullopt};
}

PollReply PollReply::Busy(const AuthorityVersion& authority)
{
    return PollReply{PollStatus::BUSY, authority, std::nullopt, std::nullopt};
}

PollReply PollReply::NeedData(const AuthorityVersion& authority)
{
    return PollReply{PollStatus::NEED_DATA, authority, std::nullopt, std::nullopt};
}

PollReply PollReply::AuthorityChanged(const AuthorityVersion& authority, const FactCursor& rewind_to)
{
    return PollReply{PollStatus::AUTHORITY_CHANGED, authority, std::nullopt, rewind_to};
}

namespace {

Error Invalid(std::string_view detail) { return Error{ErrorCode::INVALID_INPUT, detail}; }

//! True iff `to` is at most (anchor.height + 1, 0), without overflowing the height.
bool WithinAnchor(const FactCursor& to, const Anchor& anchor)
{
    if (anchor.height == std::numeric_limits<uint32_t>::max()) return true;
    const FactCursor limit{anchor.height + 1, 0};
    return !(limit < to);
}

} // namespace

Result<void> CheckBatch(const FactBatch& batch, const FactCursor& after, size_t max_facts)
{
    if (!(batch.from == after)) return Invalid(BATCH_FROM);
    if (batch.to < batch.from) return Invalid(BATCH_REVERSED);
    if (!WithinAnchor(batch.to, batch.anchor)) return Invalid(BATCH_BEYOND_ANCHOR);
    if (batch.facts.size() > max_facts || batch.facts.size() > MAX_SLOT_FACTS) return Invalid(BATCH_TOO_MANY);

    const AccountId seat_pool{SeatPoolAccount(batch.authority.profile_hash)};
    const AccountId treasury{TreasuryAccount(batch.authority.profile_hash)};
    std::set<Outpoint> outpoints;
    std::optional<FactCursor> previous;
    for (const Fact& fact : batch.facts) {
        const FactCursor cursor{CursorOf(fact)};
        if (cursor < batch.from || !(cursor < batch.to)) return Invalid(BATCH_CURSOR);
        if (previous.has_value() && !(*previous < cursor)) return Invalid(BATCH_CURSOR);
        previous = cursor;

        Result<std::vector<unsigned char>> bytes{EncodeFact(fact)};
        if (!bytes.has_value()) return bytes.error();
        if (bytes->size() > MAX_FACT_BYTES) return Error{ErrorCode::OVERSIZE, BATCH_FACT_BYTES};

        if (const auto* deposit{std::get_if<DepositFact>(&fact)}) {
            // O-34: the ledger refuses these with an error, so a slot carrying
            // one would have no valid candidate; the adapter never delivers them.
            if (deposit->amount == 0) return Invalid(BATCH_DEPOSIT_ZERO);
            if (deposit->account == seat_pool || deposit->account == treasury) return Invalid(BATCH_DEPOSIT_PROTOCOL);
            if (!outpoints.insert(deposit->outpoint).second) return Invalid(BATCH_DUPLICATE_OUTPOINT);
        }
    }
    return Ok();
}

Result<void> ChainAdapter::CheckAuthority(const AuthorityVersion& expected) const
{
    if (!(CurrentAuthority() == expected)) return Error{ErrorCode::AUTHORITY_CHANGED, CHAIN_AUTHORITY_CHANGED};
    return Ok();
}

} // namespace flowmesh2::chain
