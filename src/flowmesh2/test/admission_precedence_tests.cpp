// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** The admission precedence table (slice R12; admission/admission.h, the
 * A0..A10 table): an input that fails several checks gets the code of the
 * earliest one, and the signature is checked only when every step before
 * A8 passed.
 *
 * Each Defect makes exactly one check fail. Every single defect, every
 * pair of compatible defects, and for each defect the set of it plus every
 * later compatible defect are built into one frame (with the pool contents,
 * budget and fence the defects need) and admitted through a fresh pool and
 * admission on one shared head. The reply must carry the code of the
 * earliest defect, and sig_verifications must be 1 exactly when that
 * defect is at A8 or later (or there is none).
 */

#include <boost/test/unit_test.hpp>

#include <flowmesh2/admission/admission.h>
#include <flowmesh2/admission/pool.h>
#include <flowmesh2/core/action.h>
#include <flowmesh2/core/codes.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/test/util/admission_fixtures.h>

#include <array>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::test;

namespace {

using Frame = std::vector<unsigned char>;

//! One failing check each, in precedence order.
enum class Defect : int {
    FENCE,             //!< A0 HALTED
    FRAME_OVERSIZE,    //!< A1 OVERSIZE
    VERSION,           //!< A1 WRONG_DOMAIN (unknown version)
    TRUNCATE,          //!< A1 MALFORMED
    FOREIGN_DOMAIN,    //!< A2 WRONG_DOMAIN
    KIND_UNKNOWN,      //!< A2 UNKNOWN_KIND
    KIND_UNSUPPORTED,  //!< A2 UNSUPPORTED_KIND (WITHDRAW)
    PAYLOAD,           //!< A2 MALFORMED (payload)
    NO_ACCOUNT,        //!< A4 NO_ACCOUNT
    NONCE_LOW,         //!< A5 NONCE_TOO_LOW
    DELAY,             //!< A6 BAD_EXPIRY
    REF_AHEAD,         //!< A6 REF_SLOT_AHEAD
    WINDOW_PASSED,     //!< A6 EXPIRED
    BUDGET_DENY,       //!< A7 BUDGET
    SIGNATURE,         //!< A8 BAD_SIG
    OTHER_SIGNATURE,   //!< A9 DUPLICATE (the ActionId is pending with another signature)
    NONCE_AT_HEAD,     //!< A9 NONCE_USED
    NONCE_LIVE,        //!< A9 NONCE_PENDING
    FULL,              //!< A10 POOL_FULL
};
constexpr int DEFECT_COUNT{static_cast<int>(Defect::FULL) + 1};

constexpr std::array<AdmissionCode, DEFECT_COUNT> EXPECTED{
    AdmissionCode::HALTED,           AdmissionCode::OVERSIZE,       AdmissionCode::WRONG_DOMAIN,  AdmissionCode::MALFORMED,
    AdmissionCode::WRONG_DOMAIN,     AdmissionCode::UNKNOWN_KIND,   AdmissionCode::UNSUPPORTED_KIND, AdmissionCode::MALFORMED,
    AdmissionCode::NO_ACCOUNT,       AdmissionCode::NONCE_TOO_LOW,  AdmissionCode::BAD_EXPIRY,    AdmissionCode::REF_SLOT_AHEAD,
    AdmissionCode::EXPIRED,          AdmissionCode::BUDGET,         AdmissionCode::BAD_SIG,       AdmissionCode::DUPLICATE,
    AdmissionCode::NONCE_USED,       AdmissionCode::NONCE_PENDING,  AdmissionCode::POOL_FULL,
};

constexpr std::array<std::string_view, DEFECT_COUNT> NAMES{
    "FENCE",     "FRAME_OVERSIZE", "VERSION",       "TRUNCATE",    "FOREIGN_DOMAIN", "KIND_UNKNOWN",    "KIND_UNSUPPORTED",
    "PAYLOAD",   "NO_ACCOUNT",     "NONCE_LOW",     "DELAY",       "REF_AHEAD",      "WINDOW_PASSED",   "BUDGET_DENY",
    "SIGNATURE", "OTHER_SIGNATURE", "NONCE_AT_HEAD", "NONCE_LIVE", "FULL",
};

using DefectSet = std::set<Defect>;

//! Pairs one frame cannot carry together (they set the same field).
bool Compatible(const DefectSet& set)
{
    const auto both = [&](Defect x, Defect y) { return set.contains(x) && set.contains(y); };
    return !both(Defect::REF_AHEAD, Defect::WINDOW_PASSED) && !both(Defect::NONCE_LOW, Defect::NONCE_AT_HEAD) &&
           !both(Defect::OTHER_SIGNATURE, Defect::NONCE_LIVE);
}

std::string Describe(const DefectSet& set)
{
    if (set.empty()) return "{}";
    std::string out{"{"};
    for (const Defect defect : set) out += std::string{NAMES[static_cast<int>(defect)]} + " ";
    out.back() = '}';
    return out;
}

/** The shared head: slot 1 funds alice, bob and carol; slot 2 has alice use
 * nonces 1, 2 and 3 (window 3), so 0 is TOO_LOW and 2 is USED at head 2.
 */
struct Base {
    ProfileParams profile;
    PoolRig rig;

    static PoolRigOptions Options()
    {
        PoolRigOptions options;
        options.rig.profile = RegtestProfile();
        options.rig.profile.nonce_window = 3;
        return options;
    }

    Base() : profile{Options().rig.profile}, rig{Options()}
    {
        rig.Fund();
        SlotFixture& a{rig.Actions()};
        const AssetId rusd{TestAsset("rUSD")};
        rig.SealOriginals({}, {a.Transfer("alice", 1, KeyAccount("bob"), rusd, 1), a.Transfer("alice", 2, KeyAccount("bob"), rusd, 1),
                               a.Transfer("alice", 3, KeyAccount("bob"), rusd, 1)});
        BOOST_REQUIRE_EQUAL(rig.Height(), 2U);
    }

    //! Admit one frame carrying `defects` through a fresh pool and admission; check the code and the signature count.
    void Run(const DefectSet& defects)
    {
        BOOST_REQUIRE(Compatible(defects));
        SlotFixture& a{rig.Actions()};
        const uint64_t head{rig.Height()};
        const auto has = [&](Defect d) { return defects.contains(d); };

        const std::string_view name{has(Defect::NO_ACCOUNT) ? "dave" : "alice"};
        const uint64_t nonce{has(Defect::NONCE_LOW) ? 0U : has(Defect::NONCE_AT_HEAD) ? 2U : 10U};
        uint64_t ref{head};
        uint16_t delay{16};
        if (has(Defect::REF_AHEAD)) ref = std::numeric_limits<uint64_t>::max() - 1;
        if (has(Defect::WINDOW_PASSED)) {
            ref = 0;
            delay = 1;
        }
        if (has(Defect::DELAY)) delay = 0;
        ActionKind kind{ActionKind::PLACE};
        Frame payload{EncodePayload(PlacePayload{MarketM1().Id(), Side::BUY, TimeInForce::GTC, 1, 1}).value()};
        if (has(Defect::KIND_UNSUPPORTED)) {
            kind = ActionKind::WITHDRAW;
            payload = EncodePayload(WithdrawPayload{AssetId::Native(), 1, {0x51}}).value();
        }
        if (has(Defect::PAYLOAD)) payload.push_back(0);
        ActionV2 action{a.Unsigned(name, nonce, kind, payload, ref, delay)};
        if (has(Defect::FOREIGN_DOMAIN)) action.domain = LabelHash("fm2/test/other-domain");
        Frame frame{a.Sign(action, name)};

        // What the pool holds before the frame arrives.
        std::vector<VerifiedAction> held;
        if (has(Defect::OTHER_SIGNATURE)) held.push_back(VerifiedOf(a.Sign(action, name), profile));
        if (has(Defect::NONCE_LIVE)) {
            held.push_back(VerifiedOf(a.Sign(a.Unsigned(name, nonce, ActionKind::PLACE,
                                                        EncodePayload(PlacePayload{MarketM1().Id(), Side::BUY, TimeInForce::GTC, 7, 1}).value(),
                                                        head, 16),
                                             name),
                                      profile));
        }
        if (has(Defect::FULL)) held.push_back(VerifiedOf(a.Place("bob", 50, MarketM1(), Side::BUY, TimeInForce::GTC, 1, 1), profile));

        // Byte-level defects, after signing.
        if (has(Defect::SIGNATURE)) frame[frame.size() - ACTION_SIG_BYTES] ^= 1;
        if (has(Defect::KIND_UNKNOWN)) frame[ACTION_KIND_OFFSET] = 0x7f;
        if (has(Defect::VERSION)) frame[0] = 2;
        if (has(Defect::TRUNCATE)) frame.pop_back();
        if (has(Defect::FRAME_OVERSIZE)) frame.resize(ActionV2::MaxEncodedSize(profile) + 1, 0);

        const size_t capacity{has(Defect::FULL) ? held.size() : 64};
        std::unique_ptr<admission::ActionPool> pool{NewPool(admission::PoolLimits{capacity, 1'024, 64}, profile, head)};
        for (const VerifiedAction& entry : held) {
            const admission::InsertReply inserted{pool->Insert(entry, head)};
            BOOST_REQUIRE_MESSAGE(inserted.result == admission::InsertResult::INSERTED, "setup of " + Describe(defects));
        }
        TestBudget budget;
        budget.deny = has(Defect::BUDGET_DENY);
        std::unique_ptr<admission::Admission> admission{NewAdmission(profile, *rig.rig.owner, *rig.read, *pool, budget)};
        if (has(Defect::FENCE)) admission->Fence();

        const AdmissionCode want{defects.empty() ? AdmissionCode::ADMITTED : EXPECTED[static_cast<int>(*defects.begin())]};
        const bool signed_check{defects.empty() || *defects.begin() >= Defect::SIGNATURE};
        const Result<admission::AdmissionReply> reply{admission->Admit(frame)};
        if (!reply.has_value()) {
            BOOST_ERROR("Admit failed for " + Describe(defects));
            return;
        }
        BOOST_CHECK_MESSAGE(reply->code == want, Describe(defects) + ": got " + std::string{ToString(reply->code)} + " (" +
                                                     std::string{reply->reason} + "), want " + std::string{ToString(want)});
        BOOST_CHECK_MESSAGE(admission->Stats().sig_verifications == (signed_check ? 1U : 0U),
                            Describe(defects) + ": " + std::to_string(admission->Stats().sig_verifications) + " signature checks");
        // Only an ADMITTED reply adds to the pool.
        BOOST_CHECK_EQUAL(pool->Size(), held.size() + (reply->code == AdmissionCode::ADMITTED ? 1U : 0U));
        ++runs;
    }

    size_t runs{0};
};

} // namespace

BOOST_AUTO_TEST_SUITE(admission_precedence_tests)

BOOST_AUTO_TEST_CASE(each_defect_alone_gives_its_code)
{
    Base base;
    base.Run({});
    for (int i{0}; i < DEFECT_COUNT; ++i) base.Run({static_cast<Defect>(i)});
    BOOST_CHECK_EQUAL(base.runs, static_cast<size_t>(DEFECT_COUNT) + 1);
}

BOOST_AUTO_TEST_CASE(the_earliest_of_two_defects_wins)
{
    Base base;
    size_t skipped{0};
    for (int i{0}; i < DEFECT_COUNT; ++i) {
        for (int j{i + 1}; j < DEFECT_COUNT; ++j) {
            const DefectSet set{static_cast<Defect>(i), static_cast<Defect>(j)};
            if (!Compatible(set)) {
                ++skipped;
                continue;
            }
            base.Run(set);
        }
    }
    BOOST_CHECK_EQUAL(skipped, 3U);
    BOOST_CHECK_EQUAL(base.runs, static_cast<size_t>(DEFECT_COUNT * (DEFECT_COUNT - 1) / 2) - 3);
}

BOOST_AUTO_TEST_CASE(the_earliest_of_many_defects_wins)
{
    Base base;
    for (int i{0}; i < DEFECT_COUNT; ++i) {
        DefectSet set{static_cast<Defect>(i)};
        for (int j{i + 1}; j < DEFECT_COUNT; ++j) {
            DefectSet more{set};
            more.insert(static_cast<Defect>(j));
            if (Compatible(more)) set = std::move(more);
        }
        base.Run(set);
    }
    BOOST_CHECK_EQUAL(base.runs, static_cast<size_t>(DEFECT_COUNT));
}

BOOST_AUTO_TEST_SUITE_END()
