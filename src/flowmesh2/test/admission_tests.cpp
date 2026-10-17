// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** Admission A0..A10 (slice R12; admission/admission.h; ARCHITECTURE.md
 * section 10.1).
 *
 * Every admission code once with its counters; no signature check before
 * A8 (NO_ACCOUNT, NONCE_TOO_LOW, EXPIRED, WRONG_DOMAIN and DUPLICATE, both
 * pending and included, leave sig_verifications unchanged), BAD_SIG adds
 * one, NONCE_USED and NONCE_PENDING only after a valid signature; the
 * precedence table (an input failing several checks gets the earliest
 * code); an included action resent after its window is DUPLICATE while
 * retained; REF_SLOT_AHEAD near UINT64_MAX without overflow; HALTED after
 * Fence; UNSUPPORTED_KIND with its task ID; the payload rule equals the
 * validity predicate's; snapshot retries and BUSY; the walking skeleton's
 * admission checks; concurrent admission.
 */

#include <boost/test/unit_test.hpp>

#include <flowmesh2/admission/admission.h>
#include <flowmesh2/admission/pool.h>
#include <flowmesh2/core/action.h>
#include <flowmesh2/core/codes.h>
#include <flowmesh2/core/nonce_rule.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/engine/engine.h>
#include <flowmesh2/ledger/ledger.h>
#include <flowmesh2/test/util/admission_fixtures.h>
#include <flowmesh2/verify/b3verify.h>

#include <atomic>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::test;
using admission::AdmissionReply;
using admission::AdmissionStats;
using admission::PoolLimits;

namespace {

using Frame = std::vector<unsigned char>;

ProfileParams ProfileWith(uint32_t nonce_window, uint32_t max_delay_slots)
{
    ProfileParams profile{RegtestProfile()};
    profile.nonce_window = nonce_window;
    profile.max_delay_slots = max_delay_slots;
    profile.outcome_index_slots = max_delay_slots;
    return profile;
}

//! The profile of the code tests: a window of 3, so nonce 0 is below alice's window after Prepare.
ProfileParams WindowProfile() { return ProfileWith(3, 1'024); }

PoolRigOptions OptionsFor(const ProfileParams& profile, std::optional<PoolLimits> limits = std::nullopt, bool real_certificates = false)
{
    PoolRigOptions options;
    options.rig.profile = profile;
    options.rig.real_certificates = real_certificates;
    options.limits = limits;
    return options;
}

Frame PlacePayloadBytes(uint64_t price = 1) { return EncodePayload(PlacePayload{MarketM1().Id(), Side::BUY, TimeInForce::GTC, price, 1}).value(); }

//! A small PLACE of `name` with an explicit window.
Frame PlaceAt(SlotFixture& a, std::string_view name, uint64_t nonce, uint64_t ref_slot, uint16_t max_delay, uint64_t price = 1)
{
    return a.Sign(a.Unsigned(name, nonce, ActionKind::PLACE, PlacePayloadBytes(price), ref_slot, max_delay), name);
}

Frame BadSig(Frame bytes)
{
    bytes[bytes.size() - ACTION_SIG_BYTES] ^= 1;
    return bytes;
}

Frame Truncated(Frame bytes)
{
    bytes.pop_back();
    return bytes;
}

Frame Oversized(Frame bytes, const ProfileParams& profile)
{
    bytes.resize(ActionV2::MaxEncodedSize(profile) + 1, 0);
    return bytes;
}

Frame WithByte(Frame bytes, size_t index, unsigned char value)
{
    bytes[index] = value;
    return bytes;
}

uint256 OtherDomain() { return LabelHash("fm2/test/other-domain"); }

Frame Foreign(SlotFixture& a, std::string_view name, uint64_t nonce)
{
    ActionV2 action{a.Unsigned(name, nonce, ActionKind::PLACE, PlacePayloadBytes())};
    action.domain = OtherDomain();
    return a.Sign(action, name);
}

Frame WithdrawPayloadBytes() { return EncodePayload(WithdrawPayload{AssetId::Native(), 1, {0x51}}).value(); }

Frame KindAction(SlotFixture& a, std::string_view name, uint64_t nonce, ActionKind kind, Frame payload = {})
{
    return a.Sign(a.Unsigned(name, nonce, kind, std::move(payload)), name);
}

/** Slot 1 funds alice, bob and carol; slot 2 has alice use nonces 1, 2 and
 * 3, so with a window of 3 her nonce 0 is TOO_LOW and 2 is USED. Head 2.
 */
void Prepare(PoolRig& r)
{
    r.Fund();
    SlotFixture& a{r.Actions()};
    const AssetId rusd{TestAsset("rUSD")};
    r.SealOriginals({}, {a.Transfer("alice", 1, KeyAccount("bob"), rusd, 1), a.Transfer("alice", 2, KeyAccount("bob"), rusd, 1),
                         a.Transfer("alice", 3, KeyAccount("bob"), rusd, 1)});
    BOOST_REQUIRE_EQUAL(r.Height(), 2U);
}

//! Admit `bytes` and require `code` with exactly one more reply and `signatures` more signature checks.
AdmissionReply Step(AdmissionRig& r, const Frame& bytes, AdmissionCode code, uint64_t signatures, std::string_view what)
{
    const AdmissionStats before{r.Stats()};
    const AdmissionReply reply{r.Admit(bytes)};
    RequireCode(reply, code, what);
    RequireOneReply(before, r.Stats(), code, signatures, what);
    return reply;
}

} // namespace

BOOST_AUTO_TEST_SUITE(admission_tests)

BOOST_AUTO_TEST_CASE(create_refusals_and_domain)
{
    AdmissionRig r;
    BOOST_CHECK(r.admission->Domain() == r.Head()->Header().domain);
    BOOST_CHECK(!r.admission->Fenced());
    ProfileParams other{RegtestProfile()};
    other.fee_ppm = 200;
    RequireFailure(admission::Admission::Create(other, *r.rig.owner, *r.read, *r.pool, r.budget), ErrorCode::INVALID_INPUT,
                   admission::ADMISSION_PROFILE_MISMATCH, "profile of other components");
    // A pool of another profile.
    std::unique_ptr<admission::ActionPool> pool{NewPool(PoolLimits::FromProfile(other), other, 0)};
    RequireFailure(admission::Admission::Create(RegtestProfile(), *r.rig.owner, *r.read, *pool, r.budget), ErrorCode::INVALID_INPUT,
                   admission::ADMISSION_PROFILE_MISMATCH, "pool of another profile");
    ProfileParams main{RegtestProfile()};
    main.chain = ChainType::MAIN;
    RequireError(admission::Admission::Create(main, *r.rig.owner, *r.read, *r.pool, r.budget), ErrorCode::UNSUPPORTED_NETWORK, "MAIN");
    const AdmissionStats stats{r.Stats()};
    BOOST_CHECK_EQUAL(stats.replies, 0U);
    BOOST_CHECK_EQUAL(stats.sig_verifications, 0U);
    BOOST_CHECK_EQUAL(stats.by_code.size(), AdmissionCodeRegistry().size());
    BOOST_CHECK_EQUAL(stats.Count(static_cast<AdmissionCode>(0)), 0U);
}

BOOST_AUTO_TEST_CASE(window_refusal_without_overflow)
{
    const ProfileParams profile{RegtestProfile()};
    using admission::WindowRefusal;
    const auto code = [&](uint64_t ref, uint16_t delay, uint64_t head) -> std::optional<AdmissionCode> {
        const std::optional<admission::Refusal> refusal{WindowRefusal(ref, delay, head, profile)};
        if (!refusal) return std::nullopt;
        return refusal->code;
    };
    constexpr uint64_t MAX{std::numeric_limits<uint64_t>::max()};
    // REF_SLOT_AHEAD near UINT64_MAX, where ref_slot + max_delay would wrap.
    BOOST_CHECK(code(MAX - 1, 16, 2) == AdmissionCode::REF_SLOT_AHEAD);
    BOOST_CHECK(code(MAX, 1'024, 2) == AdmissionCode::REF_SLOT_AHEAD);
    BOOST_CHECK(code(MAX, 1'024, MAX - 1) == AdmissionCode::REF_SLOT_AHEAD);
    BOOST_CHECK(code(3, 1, 2) == AdmissionCode::REF_SLOT_AHEAD);
    // BAD_EXPIRY first.
    BOOST_CHECK(code(MAX - 1, 0, 2) == AdmissionCode::BAD_EXPIRY);
    BOOST_CHECK(code(0, 0, 9) == AdmissionCode::BAD_EXPIRY);
    BOOST_CHECK(code(2, 1'025, 2) == AdmissionCode::BAD_EXPIRY);
    BOOST_CHECK(!code(2, 1'024, 2).has_value());
    // The window edge: valid in slot ref + max_delay, expired after it.
    BOOST_CHECK(!code(5, 3, 7).has_value());                    // next slot 8 = 5 + 3
    BOOST_CHECK(code(5, 3, 8) == AdmissionCode::EXPIRED);       // next slot 9 > 8
    BOOST_CHECK(!code(5, 1, 5).has_value());                    // next slot 6
    BOOST_CHECK(code(MAX - 3, 1'024, MAX - 1) == std::nullopt); // next slot MAX < ref + 1024 (no wrap)
    BOOST_CHECK(code(MAX, 1, MAX) == AdmissionCode::EXPIRED);   // no successor of MAX
    BOOST_CHECK(WindowRefusal(MAX, 1, MAX, profile)->reason == admission::REASON_HEIGHT_EXHAUSTED);
    BOOST_CHECK(WindowRefusal(5, 3, 8, profile)->reason == admission::REASON_EXPIRED);
    // Exhaustively near both ends against a 128-bit reference.
    const std::vector<uint64_t> points{0, 1, 2, 3, 1'023, 1'024, 1'025, 2'048, MAX - 2'048, MAX - 1'025, MAX - 1'024,
                                       MAX - 1'023, MAX - 2, MAX - 1, MAX};
    const std::vector<uint16_t> delays{0, 1, 2, 1'023, 1'024, 1'025, 65'535};
    size_t checked{0};
    for (const uint64_t ref : points) {
        for (const uint16_t delay : delays) {
            for (const uint64_t head : points) {
                std::optional<AdmissionCode> want;
                if (delay < 1 || delay > profile.max_delay_slots) {
                    want = AdmissionCode::BAD_EXPIRY;
                } else if (ref > head) {
                    want = AdmissionCode::REF_SLOT_AHEAD;
                } else if (head == MAX || static_cast<unsigned __int128>(head) + 1 > static_cast<unsigned __int128>(ref) + delay) {
                    want = AdmissionCode::EXPIRED; // the head UINT64_MAX has no successor slot
                }
                BOOST_CHECK_MESSAGE(code(ref, delay, head) == want, "ref " << ref << " delay " << delay << " head " << head);
                // Admissible means the next slot is inside the core window.
                if (!want && head != MAX) BOOST_CHECK(InExpiryWindow(ref, delay, head + 1));
                ++checked;
            }
        }
    }
    BOOST_CHECK_EQUAL(checked, points.size() * delays.size() * points.size());
}

BOOST_AUTO_TEST_CASE(every_code_once_with_counters)
{
    const ProfileParams profile{WindowProfile()};
    AdmissionRig r{OptionsFor(profile, PoolLimits{1, 1'024, 16})};
    Prepare(r);
    SlotFixture& a{r.Actions()};
    const uint64_t head{r.Height()};
    const Frame good{PlaceAt(a, "alice", 10, head, 16)};

    const AdmissionReply admitted{Step(r, good, AdmissionCode::ADMITTED, 1, "ADMITTED")};
    BOOST_REQUIRE(admitted.pending.has_value());
    BOOST_CHECK(admitted.id == IdOfBytes(good, profile));
    BOOST_CHECK(admitted.pending->id == IdOfBytes(good, profile));
    BOOST_CHECK_EQUAL(admitted.pending->seq, 1U);
    BOOST_CHECK_EQUAL(admitted.pending->snapshot_height, head);
    BOOST_CHECK(admitted.head_height == head);
    BOOST_CHECK_EQUAL(admitted.reason, admission::REASON_ADMITTED);
    const AdmissionReply duplicate{Step(r, good, AdmissionCode::DUPLICATE, 0, "DUPLICATE (exact resend, pending)")};
    BOOST_CHECK(duplicate.pending == admitted.pending);
    BOOST_CHECK(!duplicate.included.has_value());
    BOOST_CHECK(!duplicate.head_height.has_value()); // A3 runs before the snapshot
    Step(r, Truncated(good), AdmissionCode::MALFORMED, 0, "MALFORMED");
    Step(r, Oversized(good, profile), AdmissionCode::OVERSIZE, 0, "OVERSIZE");
    Step(r, Foreign(a, "alice", 11), AdmissionCode::WRONG_DOMAIN, 0, "WRONG_DOMAIN");
    Step(r, WithByte(PlaceAt(a, "alice", 11, head, 16), ACTION_KIND_OFFSET, 0x7f), AdmissionCode::UNKNOWN_KIND, 0, "UNKNOWN_KIND");
    const AdmissionReply unsupported{
        Step(r, KindAction(a, "alice", 11, ActionKind::WITHDRAW, WithdrawPayloadBytes()), AdmissionCode::UNSUPPORTED_KIND, 0, "UNSUPPORTED_KIND")};
    BOOST_CHECK_EQUAL(unsupported.reason, "M2-LED-05");
    const AdmissionReply no_account{Step(r, PlaceAt(a, "dave", 1, head, 16), AdmissionCode::NO_ACCOUNT, 0, "NO_ACCOUNT")};
    BOOST_CHECK(no_account.head_height == head);
    Step(r, PlaceAt(a, "alice", 0, head, 16), AdmissionCode::NONCE_TOO_LOW, 0, "NONCE_TOO_LOW");
    Step(r, PlaceAt(a, "alice", 11, std::numeric_limits<uint64_t>::max() - 1, 16), AdmissionCode::REF_SLOT_AHEAD, 0, "REF_SLOT_AHEAD");
    Step(r, PlaceAt(a, "alice", 11, head, 0), AdmissionCode::BAD_EXPIRY, 0, "BAD_EXPIRY");
    Step(r, PlaceAt(a, "alice", 11, 0, 1), AdmissionCode::EXPIRED, 0, "EXPIRED");
    r.budget.deny = true;
    Step(r, PlaceAt(a, "alice", 11, head, 16), AdmissionCode::BUDGET, 0, "BUDGET");
    r.budget.deny = false;
    Step(r, BadSig(PlaceAt(a, "alice", 11, head, 16)), AdmissionCode::BAD_SIG, 1, "BAD_SIG");
    Step(r, PlaceAt(a, "alice", 2, head, 16), AdmissionCode::NONCE_USED, 1, "NONCE_USED");
    Step(r, PlaceAt(a, "alice", 10, head, 16, /*price=*/2), AdmissionCode::NONCE_PENDING, 1, "NONCE_PENDING");
    Step(r, PlaceAt(a, "bob", 1, head, 16), AdmissionCode::POOL_FULL, 1, "POOL_FULL");
    r.admission->Fence();
    BOOST_CHECK(r.admission->Fenced());
    Step(r, PlaceAt(a, "bob", 2, head, 16), AdmissionCode::HALTED, 0, "HALTED");

    // BUSY: a pool whose head is ahead of the owner refuses every snapshot as stale.
    TestBudget busy_budget;
    std::unique_ptr<admission::ActionPool> ahead{NewPool(PoolLimits::FromProfile(profile), profile, head + 1)};
    std::unique_ptr<admission::Admission> busy{NewAdmission(profile, *r.rig.owner, *r.read, *ahead, busy_budget)};
    const AdmissionStats busy_before{busy->Stats()};
    Result<AdmissionReply> busy_reply{busy->Admit(PlaceAt(a, "bob", 3, head, 16))};
    RequireOk(busy_reply, "BUSY admit");
    RequireCode(*busy_reply, AdmissionCode::BUSY, "BUSY");
    BOOST_CHECK_EQUAL(busy_reply->reason, admission::REASON_BUSY_SNAPSHOT);
    RequireOneReply(busy_before, busy->Stats(), AdmissionCode::BUSY, 1, "BUSY");
    BOOST_CHECK_EQUAL(busy_budget.checks.load(), 1U); // A7 and A8 are not repeated per snapshot
    BOOST_CHECK_EQUAL(busy_budget.refused.load(), 1U);
    BOOST_CHECK_EQUAL(ahead->Size(), 0U);

    // Every registered code exactly once (BUSY on the second admission), five signature checks.
    const AdmissionStats stats{r.Stats()};
    for (const CodeInfo<AdmissionCode>& info : AdmissionCodeRegistry()) {
        const uint64_t want{info.code == AdmissionCode::BUSY ? 0U : 1U};
        BOOST_CHECK_MESSAGE(stats.Count(info.code) == want, std::string{info.name} << " counted " << stats.Count(info.code));
        BOOST_CHECK_EQUAL(busy->Stats().Count(info.code), info.code == AdmissionCode::BUSY ? 1U : 0U);
    }
    BOOST_CHECK_EQUAL(stats.replies, AdmissionCodeRegistry().size() - 1);
    BOOST_CHECK_EQUAL(stats.sig_verifications, 5U);
    BOOST_CHECK_EQUAL(stats.errors, 0U);
    // The budget: checked from A7 on (6 times, one DENY); settled once per ALLOW.
    BOOST_CHECK_EQUAL(r.budget.checks.load(), 6U);
    BOOST_CHECK_EQUAL(r.budget.admitted.load(), 1U);
    BOOST_CHECK_EQUAL(r.budget.refused.load(), 4U);
    BOOST_CHECK_EQUAL(r.pool->Size(), 1U);
}

BOOST_AUTO_TEST_CASE(duplicates_pending_and_included_need_no_signature_check)
{
    AdmissionRig r{OptionsFor(RegtestProfile(), std::nullopt, /*real_certificates=*/true)};
    r.Fund();
    SlotFixture& a{r.Actions()};
    const ActionV2 unsigned_c{a.Unsigned("alice", 1, ActionKind::TRANSFER,
                                         EncodePayload(TransferPayload{KeyAccount("bob"), TestAsset("rUSD"), 5}).value())};
    const Frame c{a.Sign(unsigned_c, "alice")};
    const Frame c_other{a.Sign(unsigned_c, "alice")};
    const AdmissionReply admitted{Step(r, c, AdmissionCode::ADMITTED, 1, "admit c")};
    const AdmissionReply pending{Step(r, c, AdmissionCode::DUPLICATE, 0, "c pending")};
    BOOST_CHECK(pending.pending == admitted.pending);
    BOOST_CHECK_EQUAL(pending.reason, admission::REASON_DUPLICATE_PENDING);
    // Another valid signature over c's ActionId: verified first, then DUPLICATE of the pending entry.
    const AdmissionReply pending_other{Step(r, c_other, AdmissionCode::DUPLICATE, 1, "c other signature, pending")};
    BOOST_CHECK(pending_other.pending == admitted.pending);
    BOOST_CHECK_EQUAL(r.pool->Size(), 1U);

    PoolRig::Sealed slot{r.Seal()};
    const ActionId id{IdOfBytes(c)};
    const auto [index, certified] = FindOutcome(slot.slot.slot.outcomes, id.id);
    BOOST_CHECK(certified.code == OutcomeCode::TRANSFERRED);
    const AdmissionReply included{Step(r, c, AdmissionCode::DUPLICATE, 0, "c included")};
    BOOST_CHECK_EQUAL(included.reason, admission::REASON_DUPLICATE_INCLUDED);
    BOOST_REQUIRE(included.included.has_value());
    BOOST_CHECK(*included.included == (read::IncludedRef{2, index}));
    BOOST_REQUIRE(included.outcome.has_value());
    // The returned outcome is the certified one and verifies under the including header.
    BOOST_CHECK(included.outcome->outcome.outcome == certified);
    RequireOk(verify::VerifyCertificate(included.outcome->envelope.header, included.outcome->envelope.cert, r.rig.seats), "certificate");
    RequireOk(verify::VerifyOutcome(included.outcome->envelope.header, included.outcome->outcome.outcome, included.outcome->outcome.proof),
              "outcome");
    BOOST_CHECK(included.outcome->envelope.header.height == 2U);
    // The other signature: verified, then DUPLICATE of the included record, with the outcome.
    const AdmissionReply included_other{Step(r, c_other, AdmissionCode::DUPLICATE, 1, "c other signature, included")};
    BOOST_CHECK(included_other.included == included.included);
    BOOST_REQUIRE(included_other.outcome.has_value());
    BOOST_CHECK(included_other.outcome->outcome.outcome == certified);
    // No second execution: the pool is empty and nothing new is admitted.
    BOOST_CHECK_EQUAL(r.pool->Size(), 0U);
    BOOST_CHECK_EQUAL(r.Stats().Count(AdmissionCode::ADMITTED), 1U);
}

BOOST_AUTO_TEST_CASE(included_action_resent_after_its_window_is_duplicate_while_retained)
{
    // Retention = max_delay_slots = 2 (pool and read service).
    const ProfileParams profile{ProfileWith(100, 2)};
    AdmissionRig r{OptionsFor(profile)};
    r.Fund(); // head 1
    SlotFixture& a{r.Actions()};
    const ActionV2 unsigned_x{a.Unsigned("alice", 1, ActionKind::PLACE, PlacePayloadBytes(), /*ref_slot=*/1, /*max_delay=*/1)};
    const Frame x{a.Sign(unsigned_x, "alice")};
    const Frame x_other{a.Sign(unsigned_x, "alice")};
    Step(r, x, AdmissionCode::ADMITTED, 1, "admit x");
    r.Seal();       // slot 2 includes x
    r.Seal();       // head 3: slot 4 > 1 + 1, x's window has passed
    BOOST_REQUIRE(admission::WindowRefusal(1, 1, r.Height(), profile).has_value());
    const AdmissionReply resent{Step(r, x, AdmissionCode::DUPLICATE, 0, "x resent after its window")};
    BOOST_REQUIRE(resent.included.has_value());
    BOOST_CHECK_EQUAL(resent.included->height, 2U);
    BOOST_CHECK(resent.outcome.has_value());
    // Another signature is not an authenticated original: A6 answers first.
    Step(r, x_other, AdmissionCode::EXPIRED, 0, "other signature after the window");
    // After retention (h - s = 2) the record is gone; the window has passed for good.
    r.Seal(); // head 4
    BOOST_CHECK(!r.pool->Status(IdOfBytes(x, profile)).has_value());
    Step(r, x, AdmissionCode::EXPIRED, 0, "x after retention");
}

BOOST_AUTO_TEST_CASE(nonce_used_and_pending_only_after_a_valid_signature)
{
    const ProfileParams profile{WindowProfile()};
    AdmissionRig r{OptionsFor(profile)};
    Prepare(r);
    SlotFixture& a{r.Actions()};
    const uint64_t head{r.Height()};
    const Frame used{PlaceAt(a, "alice", 2, head, 16)};
    Step(r, BadSig(used), AdmissionCode::BAD_SIG, 1, "used nonce, bad signature");
    Step(r, used, AdmissionCode::NONCE_USED, 1, "used nonce");
    const Frame first{PlaceAt(a, "alice", 10, head, 16)};
    const Frame second{PlaceAt(a, "alice", 10, head, 16, /*price=*/3)};
    Step(r, first, AdmissionCode::ADMITTED, 1, "first of nonce 10");
    Step(r, BadSig(second), AdmissionCode::BAD_SIG, 1, "pending nonce, bad signature");
    Step(r, second, AdmissionCode::NONCE_PENDING, 1, "pending nonce");
    // The same ActionId with another signature: BAD_SIG for an invalid one, DUPLICATE for a valid one.
    const ActionV2 unsigned_s{a.Unsigned("alice", 11, ActionKind::PLACE, PlacePayloadBytes())};
    const Frame s1{a.Sign(unsigned_s, "alice")};
    const Frame s2{a.Sign(unsigned_s, "alice")};
    const AdmissionReply admitted{Step(r, s1, AdmissionCode::ADMITTED, 1, "s1")};
    Step(r, BadSig(s2), AdmissionCode::BAD_SIG, 1, "s2 with a bad signature");
    const AdmissionReply dup{Step(r, s2, AdmissionCode::DUPLICATE, 1, "s2")};
    BOOST_CHECK(dup.pending == admitted.pending);
    BOOST_CHECK_EQUAL(r.pool->Size(), 2U);
    // Neither refusal consumed anything: the first of nonce 10 is still the one pending.
    BOOST_CHECK(RequireStatus(*r.pool, IdOfBytes(first, profile)).status == admission::PoolStatus::PENDING);
    BOOST_CHECK(!r.pool->Status(IdOfBytes(second, profile)).has_value());
}

BOOST_AUTO_TEST_CASE(halted_after_fence)
{
    AdmissionRig r;
    r.Fund();
    SlotFixture& a{r.Actions()};
    const Frame x{PlaceAt(a, "alice", 1, 1, 16)};
    Step(r, x, AdmissionCode::ADMITTED, 1, "before the fence");
    r.admission->Fence();
    r.admission->Fence(); // idempotent
    const AdmissionReply exact{Step(r, x, AdmissionCode::HALTED, 0, "exact resend after the fence")};
    BOOST_CHECK(!exact.id.has_value());
    BOOST_CHECK_EQUAL(exact.reason, admission::REASON_HALTED);
    Step(r, PlaceAt(a, "bob", 1, 1, 16), AdmissionCode::HALTED, 0, "new action after the fence");
    Step(r, Frame{}, AdmissionCode::HALTED, 0, "garbage after the fence");
    BOOST_CHECK_EQUAL(r.budget.checks.load(), 1U);
    // The pool is untouched and still serves the slot builder.
    BOOST_CHECK_EQUAL(r.pool->Size(), 1U);
    PoolRig::Sealed slot{r.Seal()};
    BOOST_CHECK_EQUAL(slot.report.included, 1U);
}

BOOST_AUTO_TEST_CASE(unsupported_kinds_name_their_task)
{
    for (uint8_t raw{0}; raw <= 8; ++raw) {
        const ActionKind kind{static_cast<ActionKind>(raw)};
        const Result<void> admissible{admission::KindAdmissible(kind)};
        if (!IsKnown(kind)) {
            RequireFailure(admissible, ErrorCode::INVALID_INPUT, admission::ADMISSION_KIND_UNREGISTERED, "unregistered kind");
            continue;
        }
        BOOST_CHECK_EQUAL(admissible.has_value(), KindSupported(kind));
    }
    AdmissionRig r;
    r.Fund();
    SlotFixture& a{r.Actions()};
    const std::vector<std::pair<Frame, std::string_view>> cases{
        {KindAction(a, "alice", 1, ActionKind::WITHDRAW, WithdrawPayloadBytes()), "M2-LED-05"},
        {KindAction(a, "alice", 2, ActionKind::CANCEL_ALL), "M3-ENG-09"},
        {KindAction(a, "alice", 3, ActionKind::SPOT_TO_FUTURES), "M2-LED-06"},
        {KindAction(a, "alice", 4, ActionKind::FUTURES_TO_SPOT), "M2-LED-06"},
    };
    for (const auto& [bytes, task] : cases) {
        const AdmissionReply reply{Step(r, bytes, AdmissionCode::UNSUPPORTED_KIND, 0, task)};
        BOOST_CHECK_EQUAL(reply.reason, task);
        const Result<void> stub{admission::KindAdmissible(DecodeAction(bytes).kind)};
        BOOST_REQUIRE(!stub.has_value());
        BOOST_CHECK(stub.error().code == ErrorCode::UNSUPPORTED);
        BOOST_CHECK_EQUAL(stub.error().detail, task);
    }
    BOOST_CHECK_EQUAL(r.pool->Size(), 0U);
    // The nonces were not consumed: nonce 1 is still admissible as a PLACE.
    Step(r, PlaceAt(a, "alice", 1, 1, 16), AdmissionCode::ADMITTED, 1, "nonce 1 afterwards");
}

BOOST_AUTO_TEST_CASE(payload_rule_matches_the_validity_predicate)
{
    AdmissionRig r;
    r.Fund();
    SlotFixture& a{r.Actions()};
    const Frame place{PlacePayloadBytes()};
    const Frame cancel{EncodePayload(CancelPayload{OrderId{LabelHash("fm2/test/order")}}).value()};
    const Frame transfer{EncodePayload(TransferPayload{KeyAccount("bob"), TestAsset("rUSD"), 1}).value()};
    const auto with = [](Frame bytes, size_t index, unsigned char value) {
        bytes[index] = value;
        return bytes;
    };
    const auto plus = [](Frame bytes) {
        bytes.push_back(0);
        return bytes;
    };
    const auto minus = [](Frame bytes) {
        bytes.pop_back();
        return bytes;
    };
    Frame negative{transfer};
    for (size_t i{64}; i < 72; ++i) negative[i] = 0xff; // amount -1
    const std::vector<std::tuple<ActionKind, Frame, bool>> cases{
        {ActionKind::PLACE, place, true},
        {ActionKind::PLACE, plus(place), false},
        {ActionKind::PLACE, minus(place), false},
        {ActionKind::PLACE, {}, false},
        {ActionKind::PLACE, with(place, 32, 0), false},  // side 0
        {ActionKind::PLACE, with(place, 33, 9), false},  // time in force 9
        {ActionKind::CANCEL, cancel, true},
        {ActionKind::CANCEL, plus(cancel), false},
        {ActionKind::CANCEL, {}, false},
        {ActionKind::TRANSFER, transfer, true},
        {ActionKind::TRANSFER, negative, false},
        {ActionKind::TRANSFER, minus(transfer), false},
        {ActionKind::TRANSFER, place, false},
    };
    uint64_t nonce{1};
    for (const auto& [kind, payload, want] : cases) {
        const Frame bytes{KindAction(a, "alice", nonce++, kind, payload)};
        const ActionV2 action{DecodeAction(bytes)};
        const bool admissible{admission::PayloadAdmissible(action)};
        BOOST_CHECK_EQUAL(admissible, want);
        // The validity predicate, on the same head, agrees.
        const Result<engine::ValidatedSlot> validated{a.Validate(a.Inputs({}, {bytes}))};
        if (admissible) {
            RequireOk(validated, "a payload admission accepts");
        } else {
            RequireRefusal(validated, engine::SLOT_ACTION_PAYLOAD, "a payload admission refuses");
        }
        const AdmissionReply reply{r.Admit(bytes)};
        RequireCode(reply, admissible ? AdmissionCode::ADMITTED : AdmissionCode::MALFORMED, "admission of the payload");
        if (!admissible) BOOST_CHECK_EQUAL(reply.reason, admission::REASON_PAYLOAD_MALFORMED);
    }
    for (const ActionKind kind : {ActionKind::CANCEL_ALL, ActionKind::WITHDRAW, ActionKind::SPOT_TO_FUTURES, ActionKind::FUTURES_TO_SPOT}) {
        BOOST_CHECK(!admission::PayloadAdmissible(DecodeAction(KindAction(a, "alice", nonce++, kind))));
    }
}

BOOST_AUTO_TEST_CASE(frame_refusals_keep_their_order_under_an_unknown_kind_or_version)
{
    const ProfileParams profile{RegtestProfile()};
    AdmissionRig r;
    r.Fund();
    SlotFixture& a{r.Actions()};
    const Frame good{PlaceAt(a, "alice", 1, 1, 16)};
    Step(r, Frame{}, AdmissionCode::MALFORMED, 0, "empty frame");
    Step(r, Frame{ACTION_VERSION}, AdmissionCode::MALFORMED, 0, "version byte only");
    Step(r, WithByte(good, 0, 0), AdmissionCode::WRONG_DOMAIN, 0, "version 0");
    Step(r, Truncated(WithByte(good, 0, 2)), AdmissionCode::WRONG_DOMAIN, 0, "version 2, truncated (its layout is unknown)");
    Step(r, Oversized(WithByte(good, 0, 2), profile), AdmissionCode::OVERSIZE, 0, "version 2, oversize");
    Frame trailing{good};
    trailing.push_back(0);
    // Non-canonical CompactSize for the 50-byte payload: 0xfd 0x32 0x00 instead of 0x32.
    Frame noncanonical{good.begin(), good.begin() + ACTION_FIXED_PREFIX_BYTES};
    noncanonical.insert(noncanonical.end(), {0xfd, 0x32, 0x00});
    noncanonical.insert(noncanonical.end(), good.begin() + ACTION_FIXED_PREFIX_BYTES + 1, good.end());
    // A payload length above max_action_payload_bytes in a short frame.
    Frame long_payload{good.begin(), good.begin() + ACTION_FIXED_PREFIX_BYTES};
    long_payload.insert(long_payload.end(), {0xfd, 0x01, 0x10}); // 4,097
    long_payload.resize(long_payload.size() + 100, 0);
    const std::vector<std::pair<Frame, AdmissionCode>> malformed{{Truncated(good), AdmissionCode::MALFORMED},
                                                                 {trailing, AdmissionCode::MALFORMED},
                                                                 {noncanonical, AdmissionCode::MALFORMED},
                                                                 {long_payload, AdmissionCode::OVERSIZE},
                                                                 {Oversized(good, profile), AdmissionCode::OVERSIZE}};
    for (const auto& [frame, code] : malformed) {
        // A known kind gives the frame's code; an unknown kind byte must not hide it.
        Step(r, frame, code, 0, "known kind");
        Step(r, WithByte(frame, ACTION_KIND_OFFSET, 0x7f), code, 0, "unknown kind");
    }
    // An unknown kind with a foreign domain is WRONG_DOMAIN; with a sound frame and domain, UNKNOWN_KIND.
    Step(r, WithByte(Foreign(a, "alice", 2), ACTION_KIND_OFFSET, 0x7f), AdmissionCode::WRONG_DOMAIN, 0, "unknown kind, foreign domain");
    const AdmissionReply unknown{Step(r, WithByte(good, ACTION_KIND_OFFSET, 0), AdmissionCode::UNKNOWN_KIND, 0, "kind 0")};
    BOOST_CHECK_EQUAL(unknown.reason, admission::REASON_UNKNOWN_KIND);
    Step(r, WithByte(good, ACTION_KIND_OFFSET, 8), AdmissionCode::UNKNOWN_KIND, 0, "kind 8");
    // The frame itself is admissible afterwards.
    Step(r, good, AdmissionCode::ADMITTED, 1, "the sound frame");
}

BOOST_AUTO_TEST_CASE(a_stale_snapshot_is_checked_again)
{
    AdmissionRig r;
    r.Fund();
    SlotFixture& a{r.Actions()};
    // The head moves between the snapshot (A4..A6) and the pool insert: the budget hook commits a slot.
    bool fired{false};
    r.budget.on_check = [&] {
        if (fired) return;
        fired = true;
        r.SealOriginals({}, {});
    };
    const Frame x{PlaceAt(a, "alice", 1, 1, 16)};
    const AdmissionReply admitted{Step(r, x, AdmissionCode::ADMITTED, 1, "admitted on the second snapshot")};
    BOOST_REQUIRE(admitted.pending.has_value());
    BOOST_CHECK_EQUAL(admitted.pending->snapshot_height, 2U);
    BOOST_CHECK(admitted.head_height == 2U);
    BOOST_CHECK_EQUAL(r.budget.checks.load(), 1U);
    // Now the committed slot uses the action's nonce: the second snapshot answers NONCE_USED.
    fired = false;
    const Frame y{PlaceAt(a, "bob", 1, 2, 16)};
    r.budget.on_check = [&] {
        if (fired) return;
        fired = true;
        r.SealOriginals({}, {a.Transfer("bob", 1, KeyAccount("alice"), TestAsset("TOK"), 1)});
    };
    Step(r, y, AdmissionCode::NONCE_USED, 1, "nonce used by the slot committed meanwhile");
    r.budget.on_check = nullptr;
    // An error from the budget is an Error of Admit, counted as such; a denial is a reply.
    r.budget.fail = true;
    const AdmissionStats before{r.Stats()};
    RequireError(r.admission->Admit(PlaceAt(a, "carol", 1, r.Height(), 16)), ErrorCode::INVARIANT, "budget failure");
    BOOST_CHECK_EQUAL(r.Stats().errors, before.errors + 1);
    BOOST_CHECK_EQUAL(r.Stats().replies, before.replies);
    r.budget.fail = false;
}

BOOST_AUTO_TEST_CASE(a9_finds_an_action_included_before_the_pool_existed)
{
    // A pool created after the including slot (a restart; the pool is not persisted) holds no record,
    // so A3 cannot answer; A9 finds the inclusion in the read service's outcome index (O-71).
    AdmissionRig r;
    r.Fund();
    SlotFixture& a{r.Actions()};
    const Frame x{PlaceAt(a, "alice", 1, 1, 16)};
    r.SealOriginals({}, {x}); // head 2 includes x
    std::unique_ptr<admission::ActionPool> fresh{NewPool(PoolLimits::FromProfile(r.profile), r.profile, r.Height())};
    TestBudget budget;
    std::unique_ptr<admission::Admission> restarted{NewAdmission(r.profile, *r.rig.owner, *r.read, *fresh, budget)};
    const AdmissionStats before{restarted->Stats()};
    Result<AdmissionReply> reply{restarted->Admit(x)};
    RequireOk(reply, "Admit after a restart");
    RequireCode(*reply, AdmissionCode::DUPLICATE, "x after a restart");
    RequireOneReply(before, restarted->Stats(), AdmissionCode::DUPLICATE, 1, "x after a restart (verified first)");
    BOOST_REQUIRE(reply->included.has_value());
    BOOST_CHECK_EQUAL(reply->included->height, 2U);
    BOOST_REQUIRE(reply->outcome.has_value());
    BOOST_CHECK(reply->outcome->ref == *reply->included);
    BOOST_CHECK_EQUAL(fresh->Size(), 0U);
}

BOOST_AUTO_TEST_CASE(a_read_service_that_cannot_answer_does_not_refuse)
{
    // Before the first publication the read service answers NEED_DATA, and with every permit held BUSY;
    // neither is a refusal of the action (O-74): the snapshot still decides.
    AdmissionRig r;
    // Slot 1 is applied by the owner and shown to the pool, but not published.
    state::Candidate candidate{r.rig.Preview(r.rig.Inputs(FundingFacts(r.rig.actions), {}))};
    const SlotBody body{candidate.Body()};
    PublishableSlot slot1{r.rig.Commit(std::move(candidate))};
    RequireOk(r.pool->OnCommitted(*slot1.slot.head, slot1.slot.outcomes, body), "OnCommitted(1)");
    BOOST_REQUIRE(!r.read->PublishedHeight().has_value());
    SlotFixture& a{r.Actions()};
    const Frame first{PlaceAt(a, "alice", 1, 1, 16)};
    Step(r, first, AdmissionCode::ADMITTED, 1, "nothing published yet");
    Step(r, PlaceAt(a, "alice", 2, 1, 16), AdmissionCode::ADMITTED, 1, "nothing published yet, again");
    // Publish slot 1, then hold every reader permit.
    Publish(*r.read, slot1);
    std::vector<read::ReadHandle> held;
    for (size_t i{0}; i < r.read->Limits().max_readers; ++i) {
        Result<read::ReadHandle> handle{r.read->Acquire()};
        RequireOk(handle, "Acquire");
        held.push_back(std::move(handle).value());
    }
    RequireError(r.read->Acquire(), ErrorCode::BUSY, "every permit held");
    Step(r, PlaceAt(a, "bob", 1, 1, 16), AdmissionCode::ADMITTED, 1, "read service busy");
    Step(r, PlaceAt(a, "alice", 1, 1, 16, /*price=*/2), AdmissionCode::NONCE_PENDING, 1, "read service busy, nonce pending");
    Step(r, first, AdmissionCode::DUPLICATE, 0, "read service busy, exact resend");
}

BOOST_AUTO_TEST_CASE(walking_skeleton_admission_checks)
{
    // ARCHITECTURE.md section 14: P1, N1, N2, N3 (the pool and read side), N4, N5, N6, N8, N9, N12.
    AdmissionRig r{OptionsFor(RegtestProfile(), std::nullopt, /*real_certificates=*/true)};
    SlotFixture& a{r.Actions()};
    const AssetId rusd{TestAsset("rUSD")};
    // Slot 1: D1-D3.
    r.Seal({a.DepositBytes(KeyAccount("alice"), rusd, 1'000'000'000, "T1"),
            a.DepositBytes(KeyAccount("bob"), AssetId::Native(), 10'000'000'000, "T2"),
            a.DepositBytes(KeyAccount("bob"), TestAsset("TOK"), 500'000, "T3")});
    // Slot 2: a, b.
    const Frame order_a{a.Place("bob", 1, MarketM1(), Side::SELL, TimeInForce::GTC, 250, 5'000)};
    const Frame order_b{a.Place("alice", 1, MarketM2(), Side::BUY, TimeInForce::GTC, 600, 100'000)};
    Step(r, order_a, AdmissionCode::ADMITTED, 1, "P1 a");
    Step(r, order_b, AdmissionCode::ADMITTED, 1, "P1 b");
    PoolRig::Sealed slot2{r.Seal()};
    BOOST_CHECK(FindOutcome(slot2.slot.slot.outcomes, IdOfBytes(order_a).id).second.code == OutcomeCode::RESTING);
    BOOST_CHECK(FindOutcome(slot2.slot.slot.outcomes, IdOfBytes(order_b).id).second.code == OutcomeCode::RESTING);
    // Slot 3: N1, c, N4 pending, d.
    const Frame c{a.Place("alice", 2, MarketM1(), Side::BUY, TimeInForce::IOC, 260, 2'000)};
    const Frame d{a.Place("alice", 3, MarketM2(), Side::BUY, TimeInForce::GTC, 600, 100'000)};
    Step(r, BadSig(c), AdmissionCode::BAD_SIG, 1, "N1 c with sig[0] ^= 1");
    const AdmissionReply c_admitted{Step(r, c, AdmissionCode::ADMITTED, 1, "P1 c (nonce 2 was not consumed)")};
    const AdmissionReply c_pending{Step(r, c, AdmissionCode::DUPLICATE, 0, "N4 c pending")};
    BOOST_CHECK(c_pending.pending == c_admitted.pending);
    Step(r, d, AdmissionCode::ADMITTED, 1, "P1 d");
    PoolRig::Sealed slot3{r.Seal()};
    const auto [c_index, c_outcome] = FindOutcome(slot3.slot.slot.outcomes, IdOfBytes(c).id);
    BOOST_CHECK(c_outcome.code == OutcomeCode::FILLED);
    BOOST_CHECK(FindOutcome(slot3.slot.slot.outcomes, IdOfBytes(d).id).second.code == OutcomeCode::REJ_INSUFFICIENT_AVAILABLE);
    const AdmissionReply c_included{Step(r, c, AdmissionCode::DUPLICATE, 0, "N4 c included")};
    BOOST_REQUIRE(c_included.included.has_value());
    BOOST_CHECK(*c_included.included == (read::IncludedRef{3, c_index}));
    BOOST_REQUIRE(c_included.outcome.has_value());
    BOOST_CHECK(c_included.outcome->outcome.outcome == c_outcome);
    RequireOk(verify::VerifyOutcome(c_included.outcome->envelope.header, c_outcome, c_included.outcome->outcome.proof), "N4 outcome");
    Step(r, a.Place("alice", 2, MarketM1(), Side::BUY, TimeInForce::GTC, 1, 1), AdmissionCode::NONCE_USED, 1, "N5");
    // w: signed at head 3 with ref_slot 3 and max_delay 1.
    const Frame w{a.Sign(a.Unsigned("alice", 5, ActionKind::PLACE,
                                    EncodePayload(PlacePayload{MarketM1().Id(), Side::BUY, TimeInForce::GTC, 1, 1}).value(), 3, 1),
                         "alice")};
    // Slot 4: e, N6 e', f, w (withheld).
    const Frame e{a.Transfer("bob", 2, KeyAccount("alice"), rusd, 100'000)};
    const Frame e_prime{a.Transfer("bob", 2, KeyAccount("alice"), rusd, 1)};
    const Frame f{a.Cancel("alice", 4, a.IdOf(order_b))};
    Step(r, e, AdmissionCode::ADMITTED, 1, "P1 e");
    Step(r, e_prime, AdmissionCode::NONCE_PENDING, 1, "N6 e'");
    Step(r, f, AdmissionCode::ADMITTED, 1, "P1 f");
    Step(r, w, AdmissionCode::ADMITTED, 1, "P1 w");
    const ActionId w_id{IdOfBytes(w)};
    PoolRig::Sealed slot4{r.Seal({}, [&](const ActionV2& action) { return action.Id() == w_id; })};
    BOOST_CHECK(FindOutcome(slot4.slot.slot.outcomes, IdOfBytes(e).id).second.code == OutcomeCode::TRANSFERRED);
    BOOST_CHECK(FindOutcome(slot4.slot.slot.outcomes, IdOfBytes(f).id).second.code == OutcomeCode::CANCELLED);
    BOOST_CHECK_EQUAL(slot4.slot.slot.outcomes.size(), 2U); // only e and f; e' never entered the pool
    BOOST_CHECK(RequireStatus(*r.pool, w_id).status == admission::PoolStatus::PENDING);
    // N2: an expired PLACE needs no signature check and consumes nothing.
    Step(r, a.Sign(a.Unsigned("bob", 3, ActionKind::PLACE, PlacePayloadBytes(), 1, 2), "bob"), AdmissionCode::EXPIRED, 0, "N2");
    // Slot 5: heartbeat; the pool gives w a terminal EXPIRED and the read service proves it (N3).
    PoolRig::Sealed slot5{r.Seal()};
    BOOST_CHECK(slot5.taken.originals.empty());
    BOOST_CHECK_EQUAL(slot5.report.expired, 1U);
    BOOST_CHECK(RequireStatus(*r.pool, w_id).status == admission::PoolStatus::EXPIRED);
    Result<read::ActionReply> w_read{r.read->Action(DecodeAction(w))};
    RequireOk(w_read, "GetAction(w)");
    BOOST_REQUIRE(w_read->status == read::ActionStatus::EXPIRED);
    RequireOk(verify::VerifyExpired(DecodeAction(w), w_read->head.envelope.header, w_read->head.envelope.cert, r.rig.seats,
                                    w_read->expiry->leaf, w_read->expiry->roots, w_read->expiry->proof, r.profile),
              "N3 VerifyExpired");
    // N8, N9, N12: no signature check.
    Step(r, a.Place("carol", 1, MarketM1(), Side::BUY, TimeInForce::GTC, 1, 1), AdmissionCode::NO_ACCOUNT, 0, "N8");
    const AdmissionReply n9{Step(r, KindAction(a, "alice", 6, ActionKind::WITHDRAW, WithdrawPayloadBytes()), AdmissionCode::UNSUPPORTED_KIND,
                                 0, "N9")};
    BOOST_CHECK_EQUAL(n9.reason, "M2-LED-05");
    Step(r, Foreign(a, "alice", 7), AdmissionCode::WRONG_DOMAIN, 0, "N12");
    // 14.3: the final state, and nonce 6 was not consumed.
    CheckWalkingSkeletonFinalState(r.Head()->State(), SkeletonRun{{}, a.IdOf(order_a), a.IdOf(order_b)});
    BOOST_CHECK_EQUAL(r.Height(), 5U);
    BOOST_CHECK_EQUAL(r.pool->Size(), 0U);
}

BOOST_AUTO_TEST_CASE(concurrent_admission)
{
    AdmissionRig r;
    r.Fund();
    SlotFixture& a{r.Actions()};
    const std::vector<std::string> names{"alice", "bob", "carol"};
    std::vector<std::vector<Frame>> frames(names.size());
    for (size_t t{0}; t < names.size(); ++t) {
        for (uint64_t n{1}; n <= 32; ++n) frames[t].push_back(PlaceAt(a, names[t], n, 1, 16));
    }
    std::atomic<size_t> admitted{0};
    std::atomic<size_t> other{0};
    std::vector<std::thread> threads;
    for (size_t t{0}; t < names.size(); ++t) {
        threads.emplace_back([&, t] {
            for (const Frame& frame : frames[t]) {
                const Result<AdmissionReply> reply{r.admission->Admit(frame)};
                if (reply.has_value() && reply->code == AdmissionCode::ADMITTED) {
                    ++admitted;
                } else {
                    ++other;
                }
            }
        });
    }
    for (std::thread& thread : threads) thread.join();
    BOOST_CHECK_EQUAL(admitted.load(), 96U);
    BOOST_CHECK_EQUAL(other.load(), 0U);
    BOOST_CHECK_EQUAL(r.Stats().sig_verifications, 96U);
    BOOST_CHECK_EQUAL(r.pool->Size(), 96U);

    // Four threads submit the same 32 frames: each is admitted once, every other reply is DUPLICATE.
    AdmissionRig same;
    same.Fund();
    std::vector<Frame> shared;
    for (uint64_t n{1}; n <= 32; ++n) shared.push_back(PlaceAt(same.Actions(), "alice", n, 1, 16));
    threads.clear();
    for (int t{0}; t < 4; ++t) {
        threads.emplace_back([&] {
            for (const Frame& frame : shared) (void)same.admission->Admit(frame);
        });
    }
    for (std::thread& thread : threads) thread.join();
    const AdmissionStats stats{same.Stats()};
    BOOST_CHECK_EQUAL(stats.Count(AdmissionCode::ADMITTED), 32U);
    BOOST_CHECK_EQUAL(stats.Count(AdmissionCode::DUPLICATE), 96U);
    BOOST_CHECK_EQUAL(stats.replies, 128U);
    BOOST_CHECK_GE(stats.sig_verifications, 32U);
    BOOST_CHECK_LE(stats.sig_verifications, 128U);
    BOOST_CHECK_EQUAL(same.pool->Size(), 32U);
    BOOST_CHECK_EQUAL(same.budget.admitted.load(), 32U);
}

BOOST_AUTO_TEST_SUITE_END()
