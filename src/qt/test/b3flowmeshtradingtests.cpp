// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <chainparams.h>
#include <flowmesh/market.h>
#include <flowmesh/auth.h>
#include <flowmesh/microblock.h>
#include <key_io.h>
#include <qt/b3flowmeshtrading.h>
#include <qt/b3flowmeshmarketdata.h>
#include <qt/b3flowmeshwatch.h>
#include <core_io.h>
#include <rpc/util.h>
#include <streams.h>
#include <test/util/flowmesh.h>
#include <util/int128.h>
#include <util/moneystr.h>
#include <util/translation.h>
#include <QTest>
#include <algorithm>
#include <array>
#include <stdexcept>
#include <vector>

const TranslateFn G_TRANSLATION_FUN{nullptr};
using namespace B3FlowMeshTrading;
namespace {
uint256 H(unsigned char n) { uint256 h; h.begin()[0] = n; return h; }
UniValue Snapshot(bool paused = false)
{
    const auto id{*flowmesh::ComputeFlowMeshMarketId(H(1), H(2))};
    UniValue v{UniValue::VOBJ};
    v.pushKV("market_id", id.GetHex()); v.pushKV("base_asset_id", H(2).GetHex());
    v.pushKV("vault_id", flowmesh::ComputeFlowMeshVaultId(H(1), id)->GetHex());
    v.pushKV("domain", H(1).GetHex()); v.pushKV("execution_config_id", H(3).GetHex());
    v.pushKV("quote_asset", "B3"); v.pushKV("available", true); v.pushKV("running", true);
    v.pushKV("paused", paused); v.pushKV("pending_handoff", false); v.pushKV("halt", "none"); v.pushKV("error", "");
    v.pushKV("checkpoint_pending", true); v.pushKV("pending_checkpoint_id", H(4).GetHex());
    UniValue a{UniValue::VOBJ}; a.pushKV("account_id", H(5).GetHex()); a.pushKV("next_sequence", 7);
    a.pushKV("base_available", MAX_MONEY); a.pushKV("base_reserved", 0);
    a.pushKV("b3_available", ValueFromAmount(MAX_MONEY)); a.pushKV("b3_reserved", ValueFromAmount(0)); v.pushKV("account", a);
    return v;
}
bool Rejected(const std::function<void()>& f) {
    try { f(); } catch (const std::exception&) { return true; } catch (const UniValue&) { return true; } return false;
}
Action Order() { Action a; a.market = ParseMarket(Snapshot()); a.side = QStringLiteral("bid"); a.price = 1000; a.amount = 1'000'000; return a; }
QString Address() { uint160 n; n.begin()[0] = 1; return QString::fromStdString(EncodeDestination(PKHash{n})); }
B3FlowMeshMarketData::Units TokenUnits(int decimals = 6)
{
    B3FlowMeshMarketData::Units units;
    units.asset = QString::fromStdString(H(2).GetHex());
    units.ticker = QStringLiteral("cUSD");
    units.decimals = decimals;
    units.known = true;
    return units;
}
flowmesh::Action CanonicalOrder(const Action& action)
{
    // Same public V1 curve constructors as a canonical limit instruction;
    // Parameters below independently pins the actual wallet RPC arguments.
    flowmesh::Action result;
    result.signer = *uint256::FromHex(action.market.account.toStdString());
    result.sequence = action.market.sequence;
    const bool bid{action.side == QStringLiteral("bid")};
    result.type = static_cast<uint8_t>(bid ? flowmesh::ActionType::SUBMIT_BID : flowmesh::ActionType::SUBMIT_ASK);
    result.curve = (bid ? flowmesh::MakeLimitBidCurve(action.price, action.amount) : flowmesh::MakeLimitAskCurve(action.price, action.amount)).value();
    return result;
}
template <typename T> std::string Encoded(const T& value)
{
    DataStream stream;
    stream << value;
    return std::string{reinterpret_cast<const char*>(stream.data()), stream.size()};
}
flowmesh::FlowMeshFeeContext FeeContext()
{
    flowmesh::FlowMeshFeeContext result;
    result.market_id = H(3);
    result.treasury_owner_commitment = H(4);
    for (unsigned char i{1}; i <= 4; ++i) {
        std::array<unsigned char, 32> ikm{};
        for (size_t j{0}; j < ikm.size(); ++j) ikm[j] = static_cast<unsigned char>(i + j);
        const auto secret{bls::SecretKey::FromIKM(ikm)};
        if (!secret) throw std::runtime_error{"Unable to create generated test fee seat"};
        result.seats.push_back({H(i), secret->GetPublicKey().Compressed()});
    }
    if (!flowmesh::FlowMeshFeeContextIsCanonical(result)) throw std::runtime_error{"Invalid generated test fee context"};
    return result;
}
}
class B3FlowMeshTradingTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() { SelectParams(ChainType::REGTEST); }
    void exactTypedOrderAndSequence() {
        auto a{Order()}; const auto r{Parameters(a)};
        QCOMPARE(r.method, std::string{"submitflowmeshorder"}); QCOMPARE(r.params.size(), size_t{5});
        QVERIFY(r.params[2].isNum()); QCOMPARE(r.params[2].getInt<int64_t>(), int64_t{1000});
        QCOMPARE(r.params[3].getInt<int64_t>(), int64_t{1'000'000}); QCOMPARE(r.params[4].getInt<int64_t>(), int64_t{7});
        QVERIFY(Describe(a).contains(QStringLiteral("1.00 B3")));
        a.side = QStringLiteral("ask"); a.price = 1; a.amount = 9'007'199'254'740'993LL;
        QCOMPARE(Parameters(a).params[3].getInt<int64_t>(), a.amount);
        a.price = MAX_MONEY; QVERIFY(Rejected([&] { Parameters(a); }));
        a.price = 1; a.amount = -1; QVERIFY(Rejected([&] { Parameters(a); }));
    }
    void submissionWaitIsATrailingArgumentOfRemoteSignedActionsOnly() {
        auto order{Order()}; order.submit_wait_ms = 2000;
        const auto r{Parameters(order)};
        QCOMPARE(r.method, std::string{"submitflowmeshorder"}); QCOMPARE(r.params.size(), size_t{6});
        QCOMPARE(r.params[4].getInt<int64_t>(), int64_t{7}); QCOMPARE(r.params[5].getInt<int>(), 2000);
        order.submit_wait_ms = 0; QCOMPARE(Parameters(order).params.size(), size_t{5});
        auto cancel{Order()}; cancel.operation = Operation::Cancel; cancel.submit_wait_ms = 2000;
        const auto c{Parameters(cancel)};
        QCOMPARE(c.method, std::string{"cancelflowmeshorder"}); QCOMPARE(c.params.size(), size_t{4});
        QCOMPARE(c.params[2].getInt<int64_t>(), int64_t{7}); QCOMPARE(c.params[3].getInt<int>(), 2000);
        auto withdrawal{Order()}; withdrawal.operation = Operation::Withdraw; withdrawal.native = true;
        withdrawal.amount = 1'000'001; withdrawal.destination = Address(); withdrawal.submit_wait_ms = 2000;
        const auto w{Parameters(withdrawal)};
        QCOMPARE(w.method, std::string{"requestflowmeshwithdrawal"}); QCOMPARE(w.params.size(), size_t{6});
        QCOMPARE(w.params[4].getInt<int>(), 7); QCOMPARE(w.params[5].getInt<int>(), 2000);
        // Prepared transactions never carry it.
        auto deposit{Order()}; deposit.operation = Operation::Deposit; deposit.submit_wait_ms = 2000;
        QCOMPARE(Parameters(deposit).params.size(), size_t{4});
        // Only a remote signed action of a wallet that was already unlocked
        // waits inside the submission.
        QCOMPARE(SubmissionWaitMs(Operation::Order, true, true, 2000), 2000);
        QCOMPARE(SubmissionWaitMs(Operation::Cancel, true, true, 2000), 2000);
        QCOMPARE(SubmissionWaitMs(Operation::Withdraw, true, true, 2000), 2000);
        QCOMPARE(SubmissionWaitMs(Operation::Order, true, false, 2000), 0);
        QCOMPARE(SubmissionWaitMs(Operation::Order, false, true, 2000), 0);
        QCOMPARE(SubmissionWaitMs(Operation::Order, true, true, 0), 0);
        for (const auto operation : {Operation::Deposit, Operation::Admit, Operation::Checkpoint, Operation::Vault}) {
            QCOMPARE(SubmissionWaitMs(operation, true, true, 2000), 0);
        }
    }
    void receiptsSeparateQueueAdmissionInclusionAndExecution()
    {
        const auto a{Order()}; UniValue v{UniValue::VOBJ};
        v.pushKV("market_id", a.market.id.toStdString()); v.pushKV("account_id", a.market.account.toStdString()); v.pushKV("sequence", a.market.sequence);
        v.pushKV("action_id", H(8).GetHex()); v.pushKV("reason", "test observation"); v.pushKV("endpoint", "https://operator.invalid:18443");
        v.pushKV("certificate_verified", false); v.pushKV("outcome_verified", false);
        for (const auto* state : {"unknown", "rejected", "queued", "admitted"}) {
            v.pushKV("receipt_state", state); v.pushKV("accepted", std::string{state} == "queued" || std::string{state} == "admitted");
            CheckActionResult(v, a); const auto r{ParseReceipt(v, a.market.id)};
            QCOMPARE(r.state, QString::fromLatin1(state)); QVERIFY(!r.Included());
            QVERIFY(!DescribeReceipt(r).contains(QStringLiteral("Execution evidence verified")));
        }
        v.pushKV("receipt_state", "certified_inclusion"); v.pushKV("accepted", true);
        QVERIFY(Rejected([&] { ParseReceipt(v, a.market.id); }));
        v.pushKV("certificate_verified", true); v.pushKV("microblock_hash", H(9).GetHex()); v.pushKV("microblock_sequence", 3);
        const auto included{ParseReceipt(v, a.market.id)}; QVERIFY(included.Included()); QVERIFY(!included.outcome_verified);
        QVERIFY(DescribeReceipt(included).contains(QStringLiteral("Execution outcome is unknown")));
        v.pushKV("sequence", a.market.sequence + 1); QVERIFY(Rejected([&] { CheckActionResult(v, a); }));
        QVERIFY(Rejected([&] { ParseReceipt(v, a.market.id, QString::fromStdString(H(10).GetHex())); }));
        v.pushKV("accepted", false); QVERIFY(Rejected([&] { ParseReceipt(v, a.market.id); }));
        v.pushKV("accepted", true); v.pushKV("receipt_state", "filled"); QVERIFY(Rejected([&] { ParseReceipt(v, a.market.id); }));
    }
    void parseReceiptOutcomeStrict()
    {
        const auto a{Order()};
        UniValue v{UniValue::VOBJ};
        v.pushKV("market_id", a.market.id.toStdString()); v.pushKV("action_id", H(8).GetHex());
        v.pushKV("accepted", true); v.pushKV("receipt_state", "certified_inclusion"); v.pushKV("reason", "");
        v.pushKV("endpoint", ""); v.pushKV("certificate_verified", true); v.pushKV("outcome_verified", true);
        v.pushKV("microblock_hash", H(9).GetHex()); v.pushKV("microblock_sequence", 3);
        v.pushKV("reason_code", "APPLIED"); v.pushKV("stage", "outcome_verified");
        UniValue outcome{UniValue::VOBJ};
        outcome.pushKV("status", "applied"); outcome.pushKV("final", true); outcome.pushKV("reason_code", "APPLIED");
        outcome.pushKV("reason_text", "Executed in a certified batch."); outcome.pushKV("evidence", "certified_result_preimage");
        outcome.pushKV("microblock_sequence", 3); outcome.pushKV("microblock_hash", H(9).GetHex());
        UniValue fill{UniValue::VOBJ};
        fill.pushKV("side", "bid"); fill.pushKV("cleared", true); fill.pushKV("superseded_in_entry", false);
        fill.pushKV("quantity", 5); fill.pushKV("price", 1230); fill.pushKV("quote_atoms", 6150); fill.pushKV("fee_atoms", 0);
        fill.pushKV("net_quote_atoms", 6150);
        outcome.pushKV("fill", fill);
        v.pushKV("outcome", outcome);
        const auto r{ParseReceipt(v, a.market.id)};
        QVERIFY(r.Included()); QVERIFY(r.outcome); QVERIFY(r.Terminal()); QVERIFY(!r.NotExecuted());
        QCOMPARE(r.stage, QStringLiteral("outcome_verified")); QCOMPARE(r.reason_code, QStringLiteral("APPLIED"));
        QCOMPARE(r.outcome->fill->quantity, CAmount{5});
        // The flag and the object must agree, and the outcome must belong to
        // the verified including entry.
        auto flagless{v}; flagless.pushKV("outcome_verified", false); QVERIFY(Rejected([&] { ParseReceipt(flagless, a.market.id); }));
        auto objectless{v}; objectless.pushKV("outcome", UniValue{}); QVERIFY(Rejected([&] { ParseReceipt(objectless, a.market.id); }));
        auto other{v}; auto moved{outcome}; moved.pushKV("microblock_hash", H(10).GetHex()); other.pushKV("outcome", moved);
        QVERIFY(Rejected([&] { ParseReceipt(other, a.market.id); }));
        auto admitted{v}; admitted.pushKV("receipt_state", "admitted"); admitted.pushKV("certificate_verified", false);
        QVERIFY(Rejected([&] { ParseReceipt(admitted, a.market.id); }));
        auto status{v}; auto unknown_status{outcome}; unknown_status.pushKV("status", "filled"); status.pushKV("outcome", unknown_status);
        QVERIFY(Rejected([&] { ParseReceipt(status, a.market.id); }));
        auto quoted{v}; auto quoted_fill{fill}; quoted_fill.pushKV("quantity", "5"); auto quoted_outcome{outcome};
        quoted_outcome.pushKV("fill", quoted_fill); quoted.pushKV("outcome", quoted_outcome);
        QVERIFY(Rejected([&] { ParseReceipt(quoted, a.market.id); }));
        // An unknown reason name is shown as unrecognised and never final.
        auto novel{v}; auto novel_outcome{outcome}; novel_outcome.pushKV("reason_code", "SOMETHING_NEW"); novel.pushKV("outcome", novel_outcome);
        const auto unrecognised{ParseReceipt(novel, a.market.id)};
        QVERIFY(!unrecognised.Terminal());
        QVERIFY(unrecognised.outcome->reason_text.contains(QStringLiteral("Unrecognised reason")));
    }
    void notExecutedRequiresRejectedAndConsumer()
    {
        const auto a{Order()};
        UniValue v{UniValue::VOBJ};
        v.pushKV("market_id", a.market.id.toStdString()); v.pushKV("action_id", H(8).GetHex());
        v.pushKV("accepted", false); v.pushKV("receipt_state", "rejected"); v.pushKV("reason", "never executes");
        v.pushKV("endpoint", ""); v.pushKV("certificate_verified", false); v.pushKV("outcome_verified", true);
        UniValue outcome{UniValue::VOBJ};
        outcome.pushKV("status", "not_executed"); outcome.pushKV("final", true);
        outcome.pushKV("reason_code", "NOT_EXECUTED_SEQUENCE_CONSUMED"); outcome.pushKV("reason_text", "Never executes.");
        outcome.pushKV("evidence", "certified_sequence_consumer");
        outcome.pushKV("microblock_sequence", 4); outcome.pushKV("microblock_hash", H(11).GetHex());
        UniValue consumer{UniValue::VOBJ};
        consumer.pushKV("action_id", H(12).GetHex()); consumer.pushKV("microblock_sequence", 4); consumer.pushKV("microblock_hash", H(11).GetHex());
        outcome.pushKV("consumed_by", consumer);
        v.pushKV("outcome", outcome);
        const auto r{ParseReceipt(v, a.market.id)};
        QVERIFY(r.NotExecuted()); QVERIFY(r.Terminal()); QVERIFY(!r.Included());
        QVERIFY(DescribeReceipt(r).contains(QStringLiteral("Never executes")));
        auto queued{v}; queued.pushKV("receipt_state", "unknown");
        QVERIFY(Rejected([&] { ParseReceipt(queued, a.market.id); }));
        auto anonymous{v}; auto missing{outcome}; missing.pushKV("consumed_by", UniValue{}); anonymous.pushKV("outcome", missing);
        QVERIFY(Rejected([&] { ParseReceipt(anonymous, a.market.id); }));
    }
    void describeReceiptOutcomeTexts()
    {
        Receipt r;
        r.action_id = QString::fromStdString(H(8).GetHex());
        r.state = QStringLiteral("certified_inclusion"); r.certificate_verified = true;
        r.microblock_hash = QString::fromStdString(H(9).GetHex()); r.microblock_sequence = 812;
        Outcome o; o.status = QStringLiteral("applied"); o.final = true; o.microblock_sequence = 812;
        o.microblock_hash = r.microblock_hash; o.reason_code = QStringLiteral("APPLIED");
        o.fill = Outcome::Fill{QStringLiteral("bid"), true, false, 5, 1230, 6150, 0, 6150};
        r.outcome = o; r.outcome_verified = true;
        auto text{DescribeReceipt(r)};
        QVERIFY(text.contains(QStringLiteral("Executed in certified batch #812"))); QVERIFY(text.contains(QStringLiteral("Filled 5 at 1230")));
        QVERIFY(!text.contains(QStringLiteral("Execution outcome is unknown")));
        r.outcome->status = QStringLiteral("rejected"); r.outcome->fill.reset(); r.outcome->reason_text = QStringLiteral("Nothing to cancel on that side; sequence used.");
        QVERIFY(DescribeReceipt(r).contains(QStringLiteral("Nothing to cancel on that side")));
        // includedNotExecutedShowsWarning: not final is always said.
        r.outcome->status = QStringLiteral("included_not_executed"); r.outcome->final = false;
        r.outcome->reason_text = QStringLiteral("Included but not executed: its sequence was not next.");
        text = DescribeReceipt(r);
        QVERIFY(text.contains(QStringLiteral("not executed"))); QVERIFY(text.contains(QStringLiteral("Not final.")));
        QVERIFY(!r.Terminal());
        r.outcome.reset(); r.outcome_verified = false;
        QVERIFY(DescribeReceipt(r).contains(QStringLiteral("Execution outcome is unknown")));
    }
    void stageMappingIsNeverStrongerThanTheReceipt()
    {
        Receipt r; r.action_id = QString::fromStdString(H(8).GetHex());
        // A backend stage the receipt's evidence does not support falls back
        // to the receipt state, never upward.
        for (const auto* claimed : {"certified", "outcome_verified", "not_executed", "admitted", "queued"}) {
            r.state = QStringLiteral("unknown"); r.stage = QString::fromLatin1(claimed);
            const auto v{DescribeStage(r, false, RequestKind::Order)};
            QVERIFY(v.reached <= Stage::Signed); QCOMPARE(v.flag, StageFlag::Uncertain);
            QVERIFY(!v.terminal); QVERIFY(!v.settled); QVERIFY(v.outcome.isEmpty());
        }
        r.stage = QStringLiteral("sent");
        QCOMPARE(DescribeStage(r, false, RequestKind::Order).reached, Stage::Sent);
        r.stage = QStringLiteral("signed");
        auto v{DescribeStage(r, false, RequestKind::Order)};
        QCOMPARE(v.reached, Stage::Signed); QCOMPARE(v.flag, StageFlag::None); QVERIFY(v.status.contains(QStringLiteral("not sent yet")));
        // A certified_inclusion state without a verified certificate is not Certified.
        r.state = QStringLiteral("certified_inclusion"); r.stage = QStringLiteral("certified"); r.certificate_verified = false;
        QVERIFY(DescribeStage(r, false, RequestKind::Order).reached < Stage::Certified);
        // Reported admission is labelled as a report, not a verification.
        r.state = QStringLiteral("admitted"); r.stage = QStringLiteral("admitted");
        v = DescribeStage(r, false, RequestKind::Order);
        QCOMPARE(v.reached, Stage::Admitted); QVERIFY(v.status.contains(QStringLiteral("unverified")));
        QCOMPARE(DescribeStage(r, true, RequestKind::Order).status, QStringLiteral("Admitted by your node"));
        r.state = QStringLiteral("queued"); r.stage = QStringLiteral("queued");
        QCOMPARE(DescribeStage(r, false, RequestKind::Order).reached, Stage::Sent);
        // An outcome flag without verified inclusion is never an outcome.
        r.state = QStringLiteral("admitted"); r.outcome_verified = true;
        QVERIFY(DescribeStage(r, false, RequestKind::Order).reached < Stage::Certified);
    }
    void rejectedOrUnknownNeverBecomeNotExecutedWithoutVerifiedOutcome()
    {
        Receipt r; r.action_id = QString::fromStdString(H(8).GetHex());
        for (const auto* state : {"rejected", "unknown"}) {
            for (const auto* code : {"", "POOL_REFUSED", "NOT_EXECUTED_SEQUENCE_CONSUMED"}) {
                r.state = QString::fromLatin1(state); r.stage = r.state == QStringLiteral("rejected") ? QStringLiteral("refused") : QStringLiteral("sent");
                r.reason_code = QString::fromLatin1(code);
                const auto v{DescribeStage(r, false, RequestKind::Order)};
                QVERIFY(v.flag != StageFlag::NotExecuted); QVERIFY(!v.terminal); QVERIFY(!v.settled);
                QVERIFY(!v.status.contains(QStringLiteral("Not executed")));
                QCOMPARE(v.flag, r.state == QStringLiteral("rejected") ? StageFlag::Refused : StageFlag::Uncertain);
            }
        }
        // Only a verified consumer proof makes it terminal.
        r.state = QStringLiteral("rejected"); r.outcome_verified = true;
        Outcome proof; proof.status = QStringLiteral("not_executed"); proof.final = true;
        proof.reason_code = QStringLiteral("NOT_EXECUTED_SEQUENCE_CONSUMED"); proof.consumed_by = QString::fromStdString(H(12).GetHex());
        r.outcome = proof;
        const auto v{DescribeStage(r, false, RequestKind::Order)};
        QCOMPARE(v.flag, StageFlag::NotExecuted); QVERIFY(v.terminal); QVERIFY(v.settled);
        QVERIFY(v.status.startsWith(QStringLiteral("Not executed ("))); QCOMPARE(v.outcome, QStringLiteral("✕ not executed"));
        QVERIFY(v.reached < Stage::Certified);
    }
    void certifiedRequiresVerifiedInclusionOrRetainedProtection()
    {
        Receipt r; r.action_id = QString::fromStdString(H(8).GetHex());
        r.state = QStringLiteral("unknown"); r.no_resubmit = true;
        auto v{DescribeStage(r, false, RequestKind::Order)};
        QCOMPARE(v.reached, Stage::Certified); QCOMPARE(v.flag, StageFlag::CertifiedEarlier); QVERIFY(v.settled); QVERIFY(!v.terminal);
        QVERIFY(v.status.contains(QStringLiteral("earlier"))); QVERIFY(v.status.contains(QStringLiteral("never resent")));
        r.no_resubmit = false;
        r.state = QStringLiteral("certified_inclusion"); r.certificate_verified = true; r.stage = QStringLiteral("certified");
        r.microblock_hash = QString::fromStdString(H(9).GetHex()); r.microblock_sequence = 812;
        v = DescribeStage(r, false, RequestKind::Order);
        QCOMPARE(v.reached, Stage::Certified); QCOMPARE(v.flag, StageFlag::None); QVERIFY(v.settled); QVERIFY(!v.terminal);
        // REJECTED_BY_STATE still consumes the sequence: inclusion alone never says applied.
        QVERIFY(v.status.contains(QStringLiteral("#812"))); QVERIFY(v.status.contains(QStringLiteral("applied or rejected is not reported")));
        QCOMPARE(v.outcome, QStringLiteral("not reported"));
        // A verified outcome of the including entry: filled, resting, cancelled.
        Outcome o; o.status = QStringLiteral("applied"); o.final = true; o.microblock_sequence = 812; o.microblock_hash = r.microblock_hash;
        o.reason_code = QStringLiteral("APPLIED"); o.fill = Outcome::Fill{QStringLiteral("bid"), true, false, 250'000, 1000, 250'000'000, 25'000, 249'975'000};
        r.outcome = o; r.outcome_verified = true;
        v = DescribeStage(r, false, RequestKind::Order, 6, QStringLiteral("tUSD"));
        QCOMPARE(v.reached, Stage::OutcomeVerified); QVERIFY(v.terminal); QCOMPARE(v.outcome, QStringLiteral("✓ filled 0.25 tUSD"));
        r.outcome->fill->quantity = 0;
        QCOMPARE(DescribeStage(r, false, RequestKind::Order, 6, QStringLiteral("tUSD")).outcome, QStringLiteral("✓ no fill yet · resting"));
        r.outcome->fill.reset();
        QCOMPARE(DescribeStage(r, false, RequestKind::Cancel).outcome, QStringLiteral("✓ cancelled"));
        // A verified rejection names its reason; an included, not-final one says so.
        r.outcome->status = QStringLiteral("rejected"); r.outcome->reason_text = QStringLiteral("Insufficient certified balance");
        v = DescribeStage(r, false, RequestKind::Order);
        QCOMPARE(v.flag, StageFlag::NotExecuted); QCOMPARE(v.status, QStringLiteral("Not executed (Insufficient certified balance)")); QVERIFY(v.terminal);
        r.outcome->status = QStringLiteral("included_not_executed"); r.outcome->final = false; r.outcome->reason_text = QStringLiteral("sequence was not next");
        v = DescribeStage(r, false, RequestKind::Order);
        QCOMPARE(v.flag, StageFlag::NotFinal); QVERIFY(!v.terminal); QVERIFY(!v.settled);
        QCOMPARE(v.status, QStringLiteral("Included in certified batch #812, not executed yet · not final, may still execute (sequence was not next)"));
        QCOMPARE(v.outcome, QStringLiteral("not executed yet · may still execute"));
    }
    void onlyFinalOutcomesShowNotExecuted_data()
    {
        QTest::addColumn<QString>("status");
        QTest::addColumn<QString>("code");
        QTest::addColumn<bool>("final"); // As the node reports it.
        QTest::addColumn<bool>("definite");
        // The node's non-final outcomes: the exact bytes may still execute,
        // or the deposit still be credited, later.
        QTest::newRow("deposit not credited") << QStringLiteral("rejected") << QStringLiteral("DEPOSIT_NOT_CREDITED") << false << false;
        QTest::newRow("included bad sequence") << QStringLiteral("included_not_executed") << QStringLiteral("INCLUDED_BAD_SEQUENCE") << false << false;
        QTest::newRow("included equivocation") << QStringLiteral("included_not_executed") << QStringLiteral("INCLUDED_EQUIVOCATION") << false << false;
        // A final claim with an unrecognised reason is parsed as not final.
        QTest::newRow("unrecognised rejection") << QStringLiteral("rejected") << QStringLiteral("SOMETHING_NEW") << true << false;
        QTest::newRow("unrecognised consumer") << QStringLiteral("not_executed") << QStringLiteral("SOMETHING_NEW") << true << false;
        // Proven final outcomes.
        QTest::newRow("rejected by state") << QStringLiteral("rejected") << QStringLiteral("REJECTED_BY_STATE") << true << true;
        QTest::newRow("included malformed") << QStringLiteral("included_not_executed") << QStringLiteral("INCLUDED_MALFORMED") << true << true;
        QTest::newRow("sequence consumed") << QStringLiteral("not_executed") << QStringLiteral("NOT_EXECUTED_SEQUENCE_CONSUMED") << true << true;
    }
    void onlyFinalOutcomesShowNotExecuted()
    {
        QFETCH(QString, status); QFETCH(QString, code); QFETCH(bool, final); QFETCH(bool, definite);
        const auto a{Order()};
        const bool consumed{status == QStringLiteral("not_executed")};
        UniValue v{UniValue::VOBJ};
        v.pushKV("market_id", a.market.id.toStdString()); v.pushKV("action_id", H(8).GetHex());
        v.pushKV("accepted", !consumed); v.pushKV("receipt_state", consumed ? "rejected" : "certified_inclusion"); v.pushKV("reason", "");
        v.pushKV("endpoint", ""); v.pushKV("certificate_verified", !consumed); v.pushKV("outcome_verified", true);
        if (!consumed) { v.pushKV("microblock_hash", H(9).GetHex()); v.pushKV("microblock_sequence", 3); }
        v.pushKV("reason_code", code.toStdString()); v.pushKV("stage", consumed ? "not_executed" : "outcome_verified");
        UniValue outcome{UniValue::VOBJ};
        outcome.pushKV("status", status.toStdString()); outcome.pushKV("final", final); outcome.pushKV("reason_code", code.toStdString());
        outcome.pushKV("reason_text", "Synthetic reason."); outcome.pushKV("evidence", consumed ? "certified_sequence_consumer" : "certified_result_preimage");
        outcome.pushKV("microblock_sequence", 3); outcome.pushKV("microblock_hash", H(9).GetHex());
        if (consumed) {
            UniValue consumer{UniValue::VOBJ};
            consumer.pushKV("action_id", H(12).GetHex()); consumer.pushKV("microblock_sequence", 3); consumer.pushKV("microblock_hash", H(9).GetHex());
            outcome.pushKV("consumed_by", consumer);
        }
        v.pushKV("outcome", outcome);
        const auto r{ParseReceipt(v, a.market.id)};
        QCOMPARE(r.Terminal(), definite); QCOMPARE(r.MayStillExecute(), !definite); QCOMPARE(r.Settled(), definite);
        QCOMPARE(DescribeReceipt(r).contains(QStringLiteral("Never executes")), consumed && definite);
        for (const auto kind : {RequestKind::Order, RequestKind::Cancel, RequestKind::Other}) {
            const auto view{DescribeStage(r, false, kind)};
            QCOMPARE(view.settled, r.Settled()); QCOMPARE(view.terminal, definite);
            QCOMPARE(view.reached, consumed ? Stage::Signed : Stage::OutcomeVerified);
            if (definite) {
                QCOMPARE(view.flag, StageFlag::NotExecuted); QCOMPARE(view.outcome, QStringLiteral("✕ not executed"));
                QVERIFY(!view.status.contains(QStringLiteral("not final")));
            } else {
                // Never a definite "not executed", and never settled: it stays in view.
                QCOMPARE(view.flag, StageFlag::NotFinal); QCOMPARE(view.outcome, QStringLiteral("not executed yet · may still execute"));
                QVERIFY(view.status.contains(QStringLiteral("not final, may still execute")));
                QVERIFY(!view.status.startsWith(QStringLiteral("Not executed (")));
            }
        }
    }
    void shortTitlesUseExactUnitsAndNoRpcNames()
    {
        // Read without the view (another market's request): the action's own
        // orientation and DisplayPrice's digits.
        const TitleView none;
        auto a{Order()}; a.price = 1000; a.amount = 1'000'000; a.display_decimals = 6; a.display_ticker = QStringLiteral("tUSD");
        QCOMPARE(ShortTitle(a, none), QStringLiteral("Buy 1 tUSD @ 1 B3/tUSD"));
        a.side = QStringLiteral("ask"); QCOMPARE(ShortTitle(a, none), QStringLiteral("Sell 1 tUSD @ 1 B3/tUSD"));
        // Inverse view: a canonical ask receives B3 (Buy B3) and spends tokens.
        a.inverse_display = true; a.price = 500;
        QCOMPARE(ShortTitle(a, none), QStringLiteral("Buy B3 · spend up to 1 tUSD · limit 2 tUSD/B3"));
        // A label shows a rounded reciprocal; the review shows the exact limit.
        a.price = 1009; QCOMPARE(ShortTitle(a, none), QStringLiteral("Buy B3 · spend up to 1 tUSD · limit ≈0.99108 tUSD/B3"));
        QVERIFY(Describe(a).contains(QStringLiteral("1000/1009")));
        a.price = 500;
        a.display_decimals.reset(); a.display_ticker.clear();
        QVERIFY(ShortTitle(a, none).contains(QStringLiteral("raw units")));
        auto cancel{Order()}; cancel.operation = Operation::Cancel;
        QCOMPARE(ShortTitle(cancel, none), QStringLiteral("Cancel Buy"));
        cancel.inverse_display = true; QCOMPARE(ShortTitle(cancel, none), QStringLiteral("Cancel Sell B3"));
        // A saved instruction reads in the view: its market's verified units.
        auto units{TokenUnits()}; units.ticker = QStringLiteral("tUSD");
        TitleView view; view.market = Order().market.id; view.units = units;
        SavedAction saved; saved.market = view.market; saved.type = 0; saved.sequence = 7;
        saved.canonical_points = {{1000, 1'000'000}, {1001, 0}};
        QCOMPARE(ShortTitle(saved, view), QStringLiteral("Buy 1 tUSD @ 1 B3/tUSD"));
        saved.canonical_points = {{1000, 1'000'000}, {1005, 0}}; // not the exact limit shape: named, never re-priced
        QCOMPARE(ShortTitle(saved, view), QStringLiteral("Buy order · sequence 7"));
        view.inverse = true;
        saved.type = 3; QCOMPARE(ShortTitle(saved, view), QStringLiteral("Cancel Buy B3"));
        view.inverse = false;
        saved.type = 4; QCOMPARE(ShortTitle(saved, none), QStringLiteral("Withdrawal request"));
        QCOMPARE(KindOf(Operation::Order), RequestKind::Order); QCOMPARE(KindOf(Operation::Cancel), RequestKind::Cancel);
        QCOMPARE(KindOf(Operation::Withdraw), RequestKind::Other);
        for (const auto& title : {ShortTitle(Order(), none), ShortTitle(cancel, none), ShortTitle(saved, none)}) {
            QVERIFY(!title.contains(QStringLiteral("flowmesh"), Qt::CaseInsensitive));
        }
    }
    void requestTitlesReadTheLimitAsTheirViewShowsIt()
    {
        // Review of 2026-09-30: the request rows above Your orders read a
        // limit with DisplayPrice's digits (≈0.99701 for 1003 atoms) while
        // the order book, Your orders and the trades show ≈0.997 for it.
        using namespace B3FlowMeshMarketData;
        auto units{TokenUnits()}; units.ticker = QStringLiteral("tUSD");
        auto a{Order()}; a.price = 1003; a.amount = 250'000; a.display_decimals = 6; a.display_ticker = QStringLiteral("tUSD"); a.inverse_display = true;
        TitleView view; view.inverse = true; view.market = a.market.id; view.units = units;
        view.places = ViewPricePlaces(units, true, {1003});
        QCOMPARE(view.places, std::optional<int>{3});
        QCOMPARE(ViewPrice(1003, units, true, view.places).text, QStringLiteral("≈0.997"));
        QCOMPARE(ShortTitle(a, view), QStringLiteral("Sell B3 · receive up to 0.25 tUSD · limit ≈0.997 tUSD/B3"));
        // The saved instruction of the same limit reads the same.
        SavedAction saved; saved.market = a.market.id; saved.type = 0; saved.sequence = 8;
        saved.canonical_points = {{1003, 250'000}, {1004, 0}};
        QCOMPARE(ShortTitle(saved, view), ShortTitle(a, view));
        // The review's other prices, as ViewPrice shows them at the view's places.
        for (const auto& [price, shown] : std::vector<std::pair<CAmount, QString>>{{1004, QStringLiteral("≈0.996")}, {1009, QStringLiteral("≈0.991")}, {1429, QStringLiteral("≈0.700")}, {1000, QStringLiteral("1.000")}}) {
            a.price = price;
            QCOMPARE(ViewPrice(price, units, true, view.places).text, shown);
            QVERIFY2(ShortTitle(a, view).endsWith(QStringLiteral(" · limit %1 tUSD/B3").arg(shown)), qPrintable(ShortTitle(a, view)));
        }
        a.price = 1003;
        // A request signed in the other orientation reads in the view's.
        a.inverse_display = false;
        QCOMPARE(ShortTitle(a, view), QStringLiteral("Sell B3 · receive up to 0.25 tUSD · limit ≈0.997 tUSD/B3"));
        view.inverse = false; view.places = ViewPricePlaces(units, false, {1003});
        QCOMPARE(ShortTitle(a, view), QStringLiteral("Buy 0.25 tUSD @ 1.003 B3/tUSD"));
        QCOMPARE(ShortTitle(saved, view), ShortTitle(a, view));
        // Another market's request keeps its own orientation and digits; a
        // saved one has no verified units here.
        view.inverse = true; view.places = 3;
        a.inverse_display = true;
        view.market = QString::fromStdString(H(77).GetHex());
        QCOMPARE(ShortTitle(a, view), QStringLiteral("Sell B3 · receive up to 0.25 tUSD · limit ≈0.99701 tUSD/B3"));
        QVERIFY(ShortTitle(saved, view).contains(QStringLiteral("raw unit")));
        // So does one whose units differ from the view's.
        view.market = a.market.id; a.display_decimals = 8;
        QVERIFY(ShortTitle(a, view).contains(QStringLiteral("≈0.00997")));
    }
    void marketableAndReplaceDisclosuresArePresentationOnly()
    {
        for (const bool inverse : {false, true}) {
            auto a{Order()}; a.display_decimals = 6; a.display_ticker = QStringLiteral("tUSD"); a.inverse_display = inverse;
            a.price = 1020; a.amount = 500'000;
            const auto plain{Parameters(a)};
            a.marketable = Action::Marketable{1010, 320'000, 100, inverse ? QStringLiteral("100000/101") : QStringLiteral("1.01")};
            // Presentation only: the signed request is byte-identical.
            QCOMPARE(Parameters(a).params.write(), plain.params.write()); QCOMPARE(Parameters(a).method, plain.method);
            const auto text{Describe(a)};
            QVERIFY2(text.contains(inverse ? QStringLiteral("at 100000/101 tUSD / B3 with at most 1.00% slippage") : QStringLiteral("at 1.01 B3 / tUSD with at most 1.00% slippage")), qPrintable(text));
            QVERIFY(text.contains(QStringLiteral("0.32 tUSD (visible depth, not a fill estimate)")));
            QVERIFY(text.contains(QStringLiteral("stays as a standing order until you cancel it")));
            QVERIFY(text.contains(QStringLiteral("no market or immediate-or-cancel orders")));
            a.marketable.reset();
            QVERIFY(!Describe(a).contains(QStringLiteral("Marketable")));
            // What a new order replaces is presentation only as well.
            a.replaces = Action::Replaces{750'000, 250'000, CAmount{1000}, 2};
            QCOMPARE(Parameters(a).params.write(), plain.params.write());
            QVERIFY(Describe(a).contains(QStringLiteral("Replaces your current %1 order (0.75 tUSD left; 0.25 tUSD already filled)").arg(inverse ? QStringLiteral("Sell B3") : QStringLiteral("Buy"))));
            a.replaces->limit.reset(); a.replaces->points = 3;
            QVERIFY(Describe(a).contains(QStringLiteral("curve of 3 points (0.75 tUSD left)")));
        }
    }
    void ladderOrdersSignOneCurveAndReviewEveryPrice()
    {
        auto a{Order()}; a.display_decimals = 6; a.display_ticker = QStringLiteral("tUSD");
        a.ladder = {{1000, 300'000}, {990, 200'000}, {980, 400'000}, {979, 100'000}};
        a.price = 1000; a.amount = 1'000'000;
        const auto r{Parameters(a)};
        QCOMPARE(r.method, std::string{"submitflowmeshladder"});
        QCOMPARE(r.params.size(), size_t{4});
        QCOMPARE(r.params[1].get_str(), std::string{"bid"});
        QCOMPARE(r.params[2].size(), size_t{4});
        QCOMPARE(r.params[2][1]["price"].getInt<int64_t>(), int64_t{990});
        QCOMPARE(r.params[2][1]["quantity"].getInt<int64_t>(), int64_t{200'000});
        QCOMPARE(r.params[3].getInt<int64_t>(), int64_t{7});
        a.submit_wait_ms = 2000; QCOMPARE(Parameters(a).params.size(), size_t{5}); a.submit_wait_ms = 0;
        // The review lists every price, best first, and the exact reservation.
        const auto text{Describe(a)};
        QVERIFY2(text.contains(QStringLiteral("Ladder Buy of 4 prices, ONE standing order on this side")), qPrintable(text));
        QVERIFY(text.indexOf(QStringLiteral("0.3 tUSD at 1 B3 / tUSD")) < text.indexOf(QStringLiteral("0.1 tUSD at 0.979 B3 / tUSD")));
        QVERIFY(text.contains(QStringLiteral("Total quantity: 1 tUSD")));
        // 300,000 x 1000 + 200,000 x 990 + 400,000 x 980 + 100,000 x 979 atoms.
        QVERIFY(text.contains(QStringLiteral("Exact B3 reservation: %1 B3").arg(QString::fromStdString(FormatMoney(300'000'000 + 198'000'000 + 392'000'000 + 97'900'000)))));
        QVERIFY(text.contains(QStringLiteral("a price cannot be cancelled or edited alone")));
        QVERIFY(text.contains(QStringLiteral("Wallets before this version show it only as a curve")));
        QCOMPARE(ShortTitle(a, TitleView{}), QStringLiteral("Buy ladder · 4 prices · 1 tUSD"));
        a.replaces = Action::Replaces{500'000, 0, std::nullopt, 8, 4};
        QVERIFY(Describe(a).contains(QStringLiteral("Replaces your current Buy ladder of 4 prices (0.5 tUSD left)")));
        // The B3 view: exact reciprocal prices and token caps.
        a.inverse_display = true;
        QVERIFY(Describe(a).contains(QStringLiteral("Ladder Sell B3 of 4 prices")));
        QVERIFY(Describe(a).contains(QStringLiteral("receive up to 0.3 tUSD at no less than 1 tUSD / B3 (exact)")));
        a.inverse_display = false;
        // Refused before any call: a repeated price, a lone step, an
        // unaffordable reservation, and more than four prices.
        auto repeated{a}; repeated.ladder[1].first = 1000; QVERIFY(Rejected([&] { Parameters(repeated); }));
        auto lone{a}; lone.ladder.resize(1); QVERIFY(Rejected([&] { Parameters(lone); }));
        auto five{a}; five.ladder.emplace_back(970, 1); QVERIFY(Rejected([&] { Parameters(five); }));
        auto poor{a}; poor.market.b3_available = 100; QVERIFY(Rejected([&] { Parameters(poor); }));
        // An ask ladder reserves its quantities.
        auto ask{a}; ask.side = QStringLiteral("ask"); ask.ladder = {{1010, 100'000}, {1020, 200'000}};
        QCOMPARE(Parameters(ask).method, std::string{"submitflowmeshladder"});
        QVERIFY(Describe(ask).contains(QStringLiteral("Reserves 0.3 tUSD (the sum of the quantities)")));
        // A retained ladder is named as one, never re-priced.
        SavedAction saved; saved.market = a.market.id; saved.type = 0; saved.sequence = 9;
        const auto points{*flowmesh::MakeLadderBidCurve({{1000, 300'000}, {990, 200'000}})};
        for (const auto& point : points) saved.canonical_points.push_back({point.price, point.qty});
        TitleView view; view.market = a.market.id; view.units = TokenUnits();
        QCOMPARE(ShortTitle(saved, view), QStringLiteral("Buy ladder · 2 prices · sequence 9"));
    }
    void ladderCurvesAreRecognisedExactlyWithWhatRemains()
    {
        using B3FlowMeshMarketData::LadderStepsOf;
        using B3FlowMeshMarketData::Point;
        const auto points = [](const std::vector<flowmesh::ClearingEngine::Breakpoint>& curve) {
            std::vector<Point> out; for (const auto& point : curve) out.push_back({point.price, point.qty}); return out;
        };
        // A four-price bid ladder after a fill of 35: the best-priced lots
        // fill first (the 110 and 104 prices), 103 and 100 are whole.
        const auto bid{points(*flowmesh::MakeLadderBidCurve({{100, 10}, {103, 20}, {104, 5}, {110, 30}}))};
        const auto steps{LadderStepsOf(QStringLiteral("bid"), bid, 35)};
        QVERIFY(steps); QCOMPARE(steps->size(), size_t{4});
        QCOMPARE((*steps)[0], (B3FlowMeshMarketData::LadderStep{100, 10, 10}));
        QCOMPARE((*steps)[1], (B3FlowMeshMarketData::LadderStep{103, 20, 20}));
        QCOMPARE((*steps)[2], (B3FlowMeshMarketData::LadderStep{104, 5, 0}));
        QCOMPARE((*steps)[3], (B3FlowMeshMarketData::LadderStep{110, 30, 0}));
        // An ask ladder with a price-zero step; a limit order is one step.
        const auto ask{LadderStepsOf(QStringLiteral("ask"), points(*flowmesh::MakeLadderAskCurve({{0, 4}, {5, 6}})), 2)};
        QVERIFY(ask); QCOMPARE((*ask)[0], (B3FlowMeshMarketData::LadderStep{0, 4, 2})); QCOMPARE((*ask)[1], (B3FlowMeshMarketData::LadderStep{5, 6, 6}));
        const auto limit{LadderStepsOf(QStringLiteral("bid"), points(*flowmesh::MakeLimitBidCurve(7, 3)))};
        QVERIFY(limit); QCOMPARE(limit->size(), size_t{1});
        // Anything else is not a ladder: a slope, an equivalent but not
        // canonical staircase (a redundant flat point), a bid not ending at zero.
        QVERIFY(!LadderStepsOf(QStringLiteral("bid"), {{100, 10}, {200, 0}}));
        QVERIFY(!LadderStepsOf(QStringLiteral("bid"), {{100, 10}, {101, 5}, {105, 5}, {109, 5}, {110, 0}}));
        QVERIFY(!LadderStepsOf(QStringLiteral("bid"), {{100, 10}, {101, 5}}));
        QVERIFY(!LadderStepsOf(QStringLiteral("ask"), {{5, 4}, {6, 6}}));
        QVERIFY(!LadderStepsOf(QStringLiteral("side"), bid));
    }
    void exactReceiptRetryHasNoSigningOrEconomicArguments()
    {
        const auto market{Order().market.id}, id{QString::fromStdString(H(8).GetHex())};
        for (bool retry : {false, true}) {
            const auto r{ReceiptParameters(market, id, retry)};
            QCOMPARE(r.method, std::string{retry ? "retryflowmeshaction" : "getflowmeshactionstatus"});
            QCOMPARE(r.params.size(), size_t{2}); QCOMPARE(r.params[0].get_str(), market.toStdString()); QCOMPARE(r.params[1].get_str(), id.toStdString());
        }
        QVERIFY(Rejected([&] { ReceiptParameters(QStringLiteral("bad"), id, true); }));
        QVERIFY(Rejected([&] { ReceiptParameters(market, QString(64, QLatin1Char('0')), true); }));
        // A status read may add only a bounded wait; a retry never carries one.
        const auto waited{ReceiptParameters(market, id, false, 2000)};
        QCOMPARE(waited.method, std::string{"getflowmeshactionstatus"});
        QCOMPARE(waited.params.size(), size_t{3}); QCOMPARE(waited.params[2].getInt<int>(), 2000);
        QCOMPARE(waited.params[0].get_str(), market.toStdString()); QCOMPARE(waited.params[1].get_str(), id.toStdString());
        QCOMPARE(ReceiptParameters(market, id, true, 2000).params.size(), size_t{2});
        QCOMPARE(ReceiptParameters(market, id, false, 0).params.size(), size_t{2});
        QCOMPARE(ReceiptParameters(market, id, false, -1).params.size(), size_t{2});
        UniValue legacy{UniValue::VOBJ}; legacy.pushKV("market_id", market.toStdString()); legacy.pushKV("action_id", id.toStdString()); legacy.pushKV("accepted", true);
        const auto receipt{ParseReceipt(legacy, market)}; QCOMPARE(receipt.state, QStringLiteral("queued")); QVERIFY(!receipt.Included());
    }
    void previouslyCertifiedWithoutFreshProofIsNotNeverEstablished()
    {
        Receipt receipt;
        receipt.action_id = QString::fromStdString(H(8).GetHex());
        receipt.state = QStringLiteral("unknown");
        QVERIFY(DescribeReceipt(receipt).contains(QStringLiteral("Submission outcome has never been established")));
        QVERIFY(!DescribeReceipt(receipt).contains(QStringLiteral("Previously certified;")));
        for (const auto* state : {"unknown", "queued", "admitted", "rejected"}) {
            receipt.state = QString::fromLatin1(state);
            receipt.no_resubmit = true; // Only the retained verified local record supplies this flag.
            const auto text{DescribeReceipt(receipt)};
            QVERIFY(text.contains(QStringLiteral("Previously certified; fresh proof currently unavailable")));
            QVERIFY(!text.contains(QStringLiteral("Submission outcome has never been established")));
            QVERIFY(!text.contains(QStringLiteral("Exact action inclusion verified")));
            QVERIFY(!receipt.Included());
        }
        receipt.state = QStringLiteral("certified_inclusion");
        receipt.certificate_verified = true;
        receipt.microblock_hash = QString::fromStdString(H(9).GetHex());
        receipt.microblock_sequence = 21;
        QVERIFY(receipt.Included());
        QVERIFY(DescribeReceipt(receipt).contains(QStringLiteral("Exact action inclusion verified")));
        QVERIFY(!DescribeReceipt(receipt).contains(QStringLiteral("fresh proof currently unavailable")));
    }
    void savedInstructionsRestoreIdentityWithoutTrustingPriorCertification()
    {
        const auto market{Order().market};
        UniValue receipt{UniValue::VOBJ}; receipt.pushKV("market_id", market.id.toStdString()); receipt.pushKV("account_id", market.account.toStdString());
        receipt.pushKV("action_id", H(8).GetHex()); receipt.pushKV("receipt_state", "unknown"); receipt.pushKV("accepted", false);
        receipt.pushKV("reason", "Retained original action; fresh status evidence required after restart"); receipt.pushKV("endpoint", "");
        receipt.pushKV("certificate_verified", false); receipt.pushKV("outcome_verified", false);
        UniValue row{UniValue::VOBJ}; row.pushKV("market_id", market.id.toStdString()); row.pushKV("domain", market.domain.toStdString());
        row.pushKV("execution_config_id", market.config.toStdString()); row.pushKV("account_id", market.account.toStdString()); row.pushKV("action_id", H(8).GetHex());
        row.pushKV("action_type", 0); row.pushKV("sequence", 7); row.pushKV("signed_bytes_sha256", H(9).GetHex()); row.pushKV("signed_bytes_size", 279);
        row.pushKV("initial_submission_ms", 1000); row.pushKV("may_have_been_sent", true); row.pushKV("previously_certified", false); row.pushKV("receipt", receipt);
        const auto response = [&](const UniValue& item, size_t count = 1) {
            UniValue root{UniValue::VOBJ}, actions{UniValue::VARR}; root.pushKV("source", "local-retained-outbox"); root.pushKV("account_id", market.account.toStdString());
            for (size_t i{0}; i < count; ++i) actions.push_back(item);
            root.pushKV("actions", actions); return root;
        };
        const auto parsed{ParseSavedActions(response(row))}; QCOMPARE(parsed.actions.size(), size_t{1});
        const auto& action{parsed.actions.front()}; QCOMPARE(action.sequence, std::optional<uint64_t>{7});
        QCOMPARE(action.receipt.action_id, QString::fromStdString(H(8).GetHex())); QCOMPARE(action.signed_bytes_sha256, QString::fromStdString(H(9).GetHex()));
        QVERIFY(action.may_have_been_sent); QVERIFY(!action.receipt.no_resubmit); QVERIFY(!action.receipt.Included());
        QVERIFY(DescribeSavedAction(action).contains(QStringLiteral("Original account sequence: 7")));
        row.pushKV("previously_certified", true); const auto prior{ParseSavedActions(response(row))};
        QVERIFY(prior.actions[0].receipt.no_resubmit); QVERIFY(!prior.actions[0].receipt.Included());
        QVERIFY(DescribeSavedAction(prior.actions[0]).contains(QStringLiteral("no-resubmit protection")));
        QVERIFY(DescribeSavedAction(prior.actions[0]).contains(QStringLiteral("fresh verification label requires current evidence")));
        QVERIFY(Rejected([&] { ParseSavedActions(response(row, 2)); }));
        QVERIFY(Rejected([&] { ParseSavedActions(response(row, 513)); }));
        auto wrong{row}; wrong.pushKV("account_id", H(10).GetHex()); QVERIFY(Rejected([&] { ParseSavedActions(response(wrong)); }));
        wrong = row; auto wrong_receipt{receipt}; wrong_receipt.pushKV("market_id", H(11).GetHex()); wrong.pushKV("receipt", wrong_receipt);
        QVERIFY(Rejected([&] { ParseSavedActions(response(wrong)); }));
        wrong = row; wrong.pushKV("signed_bytes_size", 4097); QVERIFY(Rejected([&] { ParseSavedActions(response(wrong)); }));
        wrong = row; wrong.pushKV("sequence", "7"); QVERIFY(Rejected([&] { ParseSavedActions(response(wrong)); }));
        wrong = row; wrong.pushKV("action_type", 5); QVERIFY(Rejected([&] { ParseSavedActions(response(wrong)); }));
        wrong = row; wrong.pushKV("receipt", UniValue{UniValue::VNULL}); QVERIFY(Rejected([&] { ParseSavedActions(response(wrong)); }));
        UniValue empty{UniValue::VOBJ}; empty.pushKV("source", "local-retained-outbox"); empty.pushKV("actions", UniValue{UniValue::VARR});
        const auto no_account{ParseSavedActions(empty)}; QVERIFY(no_account.account.isEmpty()); QVERIFY(no_account.actions.empty());
    }
    void reciprocalPricesMapToExactCanonicalIntegers()
    {
        using namespace B3FlowMeshMarketData;
        const auto units{TokenUnits()};
        QCOMPARE(ParseDisplayPrice(QStringLiteral("2"), units, true).value(), CAmount{500});
        QCOMPARE(ParseDisplayPrice(QStringLiteral("0.5"), units, true).value(), CAmount{2000});
        QCOMPARE(ParseDisplayPrice(QStringLiteral("1000/901"), units, true).value(), CAmount{901});
        QCOMPARE(ParseDisplayPrice(QStringLiteral("2000/1802"), units, true).value(), CAmount{901});
        QCOMPARE(FormatDisplayPrice(500, 6, true), QStringLiteral("2"));
        QCOMPARE(FormatDisplayPrice(2000, 6, true), QStringLiteral("0.5"));
        QCOMPARE(FormatDisplayPrice(500, 6, false), FormatPrice(500, 6));
        // The reciprocal of adjacent canonical ticks is not a uniform
        // decimal grid. Preserve the exact fraction instead of rounding it.
        QCOMPARE(ExactInversePrice(500, 6), QStringLiteral("2"));
        QCOMPARE(ExactInversePrice(501, 6), QStringLiteral("1000/501"));
        QCOMPARE(ExactInversePrice(502, 6), QStringLiteral("500/251"));
        QCOMPARE(ExactInversePrice(901, 6), QStringLiteral("1000/901"));
        const auto approximate{FormatDisplayPrice(901, 6, true)};
        QCOMPARE(approximate, QStringLiteral("≈1.11"));
        QVERIFY(!ParseDisplayPrice(approximate, units, true));
        QVERIFY(!ParseDisplayPrice(QStringLiteral("1.1"), units, true));
        QVERIFY(!ParseDisplayPrice(QStringLiteral("1.996"), units, true));
        QVERIFY(!ParseDisplayPrice(QStringLiteral("1000/901"), units, false));
        auto stepped{units}; stepped.price_step = 7;
        QVERIFY(!ParseDisplayPrice(QStringLiteral("2"), stepped, true));
        QCOMPARE(ParseDisplayPrice(QStringLiteral("1000/497"), stepped, true).value(), CAmount{497});
        stepped.quantity_step = 7;
        QVERIFY(!ParseQuantity(QStringLiteral("1"), stepped));
        QCOMPARE(ParseQuantity(QStringLiteral("0.000007"), stepped).value(), CAmount{7});
    }
    void reciprocalExtremesRoundTripWithoutFloatingPoint()
    {
        using namespace B3FlowMeshMarketData;
        for (int decimals : {0, 6, 9, 18}) {
            const auto units{TokenUnits(decimals)};
            for (CAmount raw : {CAmount{1}, CAmount{2}, CAmount{3}, CAmount{901}, CAmount{1000}, MAX_MONEY - 1, MAX_MONEY}) {
                const auto exact{ExactInversePrice(raw, decimals)};
                const auto parsed{ParseDisplayPrice(exact, units, true)};
                QVERIFY2(parsed.has_value(), qPrintable(exact));
                QCOMPARE(*parsed, raw);
                QVERIFY(!FormatDisplayPrice(raw, decimals, true).isEmpty());
            }
        }
        QCOMPARE(ExactInversePrice(1, 0), QStringLiteral("1000000000"));
        QCOMPARE(ExactInversePrice(1, 18), QStringLiteral("0.000000001"));
        for (CAmount raw : {CAmount{-1}, CAmount{0}, MAX_MONEY + 1}) {
            QCOMPARE(ExactInversePrice(raw, 6), QStringLiteral("—"));
            QCOMPARE(FormatDisplayPrice(raw, 6, true), QStringLiteral("—"));
        }
        QCOMPARE(ExactInversePrice(1, -1), QStringLiteral("—"));
        QCOMPARE(ExactInversePrice(1, 19), QStringLiteral("—"));
        const auto units{TokenUnits()};
        for (const auto& text : {QStringLiteral(""), QStringLiteral("0"), QStringLiteral("0.0"),
                 QStringLiteral("-1"), QStringLiteral("+1"), QStringLiteral("NaN"), QStringLiteral("inf"),
                 QStringLiteral("1e3"), QStringLiteral(" 1"), QStringLiteral("1 "), QStringLiteral("1."),
                 QStringLiteral("1/0"), QStringLiteral("0/1"), QStringLiteral("1/-1"), QStringLiteral("1/1.0"),
                 QStringLiteral("1//1"), QStringLiteral("1000000000000000000000000000000000000000/1")}) {
            QVERIFY2(!ParseDisplayPrice(text, units, true), qPrintable(text));
        }
        QVERIFY(!ParseDisplayPrice(QStringLiteral("0.000000000000000001"), TokenUnits(0), true));
        auto unknown{units}; unknown.known = false;
        QVERIFY(!ParseDisplayPrice(QStringLiteral("2"), unknown, true));
        unknown = units; unknown.decimals = 19;
        QVERIFY(!ParseDisplayPrice(QStringLiteral("2"), unknown, true));
    }
    void displayPricesShareOneBoundedFormatter()
    {
        using namespace B3FlowMeshMarketData;
        const auto units{TokenUnits()};
        // Token per B3: one digit more than price / step tells a price from its
        // neighbouring ticks; half up, trailing zeros trimmed, exact kept.
        const auto last{DisplayPrice(1009, units, true)};
        QCOMPARE(last.text, QStringLiteral("≈0.99108")); QCOMPARE(last.exact, QStringLiteral("1000/1009")); QVERIFY(last.approximate);
        QCOMPARE(DisplayPrice(1010, units, true).text, QStringLiteral("≈0.9901"));
        QCOMPARE(DisplayPrice(1001, units, true).text, QStringLiteral("≈0.999"));
        QCOMPARE(DisplayPrice(999, units, true).text, QStringLiteral("≈1.001"));
        QCOMPARE(FormatDisplayPrice(1009, units, true), last.text);
        for (const CAmount raw : {CAmount{500}, CAmount{800}, CAmount{1000}, CAmount{2000}}) {
            const auto exact{DisplayPrice(raw, units, true)};
            QVERIFY(!exact.approximate); QCOMPARE(exact.text, exact.exact); QCOMPARE(exact.exact, ExactInversePrice(raw, 6));
        }
        // Never rounded into the integer part; a carry runs through nines.
        QCOMPARE(DisplayPrice(3, units, true).text, QStringLiteral("≈333"));
        QCOMPARE(DisplayPrice(6, units, true).text, QStringLiteral("≈167"));
        QCOMPARE(DisplayPrice(7, 0, true, 1).text, QStringLiteral("≈142857143"));
        QCOMPARE(DisplayPrice(1'000'001, 6, true, 1000).text, QStringLiteral("≈0.001"));
        QCOMPARE(DisplayPrice(1'000'001, 0, true, 10'000).text, QStringLiteral("≈1000"));
        // The verified step sets the precision, capped; without one at most 6.
        QCOMPARE(DisplayPrice(123'456'789, 0, true, 1).text, QStringLiteral("≈8.1000001"));
        QCOMPARE(DisplayPrice(123'456'789, 0, true, 1'000'000).text, QStringLiteral("≈8.1"));
        QCOMPARE(DisplayPrice(123'456'789, 0, true).text, QStringLiteral("≈8.1"));
        auto unverified{units}; unverified.known = false;
        QCOMPARE(DisplayPrice(123'456'789, unverified, true), DisplayPrice(123'456'789, 6, true));
        // Too small for 18 places: the exact form, never "≈0".
        QVERIFY(!DisplayPrice(MAX_MONEY, 18, true).approximate);
        QCOMPARE(DisplayPrice(MAX_MONEY, 18, true).text, ExactInversePrice(MAX_MONEY, 18));
        // B3 per token is always exact at the asset precision.
        for (const CAmount raw : {CAmount{1}, CAmount{1009}, CAmount{123'456'789}, MAX_MONEY}) {
            for (const int decimals : {0, 6, 9, 18}) {
                const auto p{DisplayPrice(raw, TokenUnits(decimals), false)};
                QVERIFY(!p.approximate); QCOMPARE(p.text, FormatPrice(raw, decimals)); QCOMPARE(p.exact, p.text);
            }
        }
        for (const auto& invalid : {DisplayPrice(0, units, true), DisplayPrice(-1, units, true), DisplayPrice(MAX_MONEY + 1, units, true), DisplayPrice(1, 19, true), DisplayPrice(-1, units, false)}) {
            QCOMPARE(invalid.text, QStringLiteral("—")); QVERIFY(!invalid.approximate);
        }
        // Display text never becomes an order input; the exact form does.
        QVERIFY(!ParseDisplayPrice(last.text, units, true)); QVERIFY(!ParseDisplayLimit(last.text, units, true, true));
        QCOMPARE(ParseDisplayPrice(last.exact, units, true).value(), CAmount{1009});
        // Bounded everywhere: at most MAX_PRICE_SIGNIFICANT_DIGITS unless the
        // integer part needs more, and at most 18 decimal places.
        for (const int decimals : {0, 6, 9, 18}) {
            for (const CAmount step : {CAmount{0}, CAmount{1}, CAmount{7}}) {
                for (const CAmount raw : {CAmount{1}, CAmount{2}, CAmount{3}, CAmount{7}, CAmount{901}, CAmount{1009}, CAmount{123'456'789}, MAX_MONEY - 1, MAX_MONEY}) {
                    const auto p{DisplayPrice(raw, decimals, true, step)};
                    QCOMPARE(ParseDisplayPrice(p.exact, TokenUnits(decimals), true).value(), raw);
                    if (!p.approximate) { QCOMPARE(p.text, p.exact); continue; }
                    QVERIFY(p.text.startsWith(QStringLiteral("≈")));
                    const QString number{p.text.mid(1)};
                    const QString integer{number.section(QLatin1Char('.'), 0, 0)}, fraction{number.section(QLatin1Char('.'), 1)};
                    QVERIFY(fraction.size() <= 18); QVERIFY(!fraction.endsWith(QLatin1Char('0')));
                    QString significant{integer == QStringLiteral("0") ? fraction : integer + fraction};
                    while (significant.startsWith(QLatin1Char('0'))) significant.remove(0, 1);
                    const int bound{step > 0 ? MAX_PRICE_SIGNIFICANT_DIGITS : FALLBACK_PRICE_SIGNIFICANT_DIGITS};
                    QVERIFY2(significant.size() <= std::max<qsizetype>(bound, integer == QStringLiteral("0") ? 0 : integer.size()), qPrintable(p.text));
                }
            }
        }
    }
    void sideAwareLimitsPreserveTheExistingThirtyTwoVectorStudy()
    {
        using namespace B3FlowMeshMarketData;
        // Price and direction cases from the preserved 32-vector arithmetic
        // note now run through the production adapter, not a Python model.
        struct Row { int decimals; const char* input; CAmount buy; CAmount sell; };
        for (const Row row : {Row{6, "2", 500, 500}, Row{6, "0.5", 2000, 2000},
                 Row{6, "1000/901", 901, 901}, Row{6, "1.1", 910, 909},
                 Row{0, "10", 100'000'000, 100'000'000}, Row{9, "0.5", 2, 2},
                 Row{18, "0.000000001", 1, 1}, Row{18, "2", 1, 0}}) {
            for (bool buy : {false, true}) {
                const auto limit{ParseDisplayLimit(QString::fromLatin1(row.input), TokenUnits(row.decimals), true, buy)};
                const CAmount expected{buy ? row.buy : row.sell};
                QCOMPARE(limit.has_value(), expected != 0);
                if (!limit) continue;
                QCOMPARE(limit->price, expected);
                QCOMPARE(limit->executable_price, ExactInversePrice(expected, row.decimals));
                QCOMPARE(ParseDisplayPrice(limit->executable_price, TokenUnits(row.decimals), true).value(), expected);
                QCOMPARE(limit->adjusted, !ParseDisplayPrice(QString::fromLatin1(row.input), TokenUnits(row.decimals), true).has_value());
            }
        }
        const auto buy{ParseDisplayLimit(QStringLiteral("1.1"), TokenUnits(), true, true)};
        const auto sell{ParseDisplayLimit(QStringLiteral("1.1"), TokenUnits(), true, false)};
        QVERIFY(buy && sell && buy->adjusted && sell->adjusted);
        QCOMPARE(buy->executable_price, QStringLiteral("100/91"));
        QCOMPARE(sell->executable_price, QStringLiteral("1000/909"));
        // A coarser caller-supplied grid is treated conservatively too;
        // certified V1 market parsing still rejects any price_step != 1.
        auto stepped{TokenUnits()}; stepped.price_step = 7;
        QCOMPARE(ParseDisplayLimit(QStringLiteral("2"), stepped, true, true)->price, CAmount{504});
        QCOMPARE(ParseDisplayLimit(QStringLiteral("2"), stepped, true, false)->price, CAmount{497});
        QCOMPARE(ParseDisplayLimit(QStringLiteral("1000/497"), stepped, true, true)->adjusted, false);
    }
    void conservativeLimitPropertiesHoldAcrossDecimalAndPriceGrids()
    {
        using namespace B3FlowMeshMarketData;
        using Wide = util::Unsigned128;
        size_t checked{0};
        for (int decimals : {0, 2, 6, 9, 12, 18}) {
            Wide scale{1}; for (int i{0}; i < decimals; ++i) scale *= 10;
            for (CAmount step : {CAmount{1}, CAmount{7}}) {
                auto units{TokenUnits(decimals)}; units.price_step = step;
                for (uint64_t numerator{1}; numerator <= 121; ++numerator) {
                    for (uint64_t denominator : {uint64_t{1}, uint64_t{3}, uint64_t{7}, uint64_t{10}, uint64_t{37}, uint64_t{101}, uint64_t{997}}) {
                        const auto text{QStringLiteral("%1/%2").arg(numerator).arg(denominator)};
                        const Wide rhs{Wide{KILO_COIN} * denominator};
                        const Wide divisor{scale * numerator};
                        for (bool buy : {false, true}) {
                            const auto limit{ParseDisplayLimit(text, units, true, buy)};
                            if (!limit) { QVERIFY(!buy && rhs < divisor * static_cast<uint64_t>(step)); continue; }
                            const Wide lhs{static_cast<uint64_t>(limit->price) * divisor};
                            QVERIFY(limit->price > 0 && limit->price % step == 0);
                            // These are cross products of exact rational
                            // prices, including the decimal unit conversion.
                            QVERIFY(buy ? lhs >= rhs : lhs <= rhs);
                            if (buy) QVERIFY(static_cast<uint64_t>(limit->price - step) * divisor < rhs);
                            else QVERIFY(static_cast<uint64_t>(limit->price + step) * divisor > rhs);
                            QCOMPARE(ParseDisplayPrice(limit->executable_price, units, true).value(), limit->price);
                            ++checked;
                        }
                    }
                }
            }
        }
        QVERIFY(checked > 10'000);
    }
    void sideAwareLimitBoundariesRejectWithoutWrappingOrRelaxing()
    {
        using namespace B3FlowMeshMarketData;
        const auto units{TokenUnits()};
        for (bool buy : {false, true}) {
            for (const auto* text : {"", "0", "0.0", "-1", "+1", "NaN", "inf", "1e3", " 1", "1 ", "1.", "1/0", "0/1", "1/-1", "1/1.0", "1//1", "1000000000000000000000000000000000000000/1"}) {
                QString error;
                QVERIFY(!ParseDisplayLimit(QString::fromLatin1(text), units, true, buy, &error));
                QVERIFY(!error.isEmpty());
            }
            QVERIFY(!ParseDisplayLimit(QStringLiteral("≈1.1"), units, true, buy));
            QVERIFY(!ParseDisplayLimit(QStringLiteral("0.000000000000000001"), TokenUnits(0), true, buy));
            QVERIFY(!ParseDisplayLimit(QStringLiteral("1/99999999999999999999999999999999999999"), TokenUnits(0), true, buy));
            QVERIFY(!ParseDisplayLimit(QStringLiteral("99999999999999999999999999999999999999/3"), TokenUnits(18), true, buy));
            auto unknown{units}; unknown.known = false;
            QVERIFY(!ParseDisplayLimit(QStringLiteral("2"), unknown, true, buy));
            unknown = units; unknown.decimals = 19;
            QVERIFY(!ParseDisplayLimit(QStringLiteral("2"), unknown, true, buy));
        }
        const auto max_text{ExactInversePrice(MAX_MONEY, 6)};
        QCOMPARE(ParseDisplayLimit(max_text, units, true, true)->price, MAX_MONEY);
        QVERIFY(!ParseDisplayLimit(max_text, units, true, false)); // bid endpoint would overflow
        QCOMPARE(ParseDisplayLimit(ExactInversePrice(MAX_MONEY - 1, 6), units, true, false)->price, MAX_MONEY - 1);
        QCOMPARE(ParseDisplayLimit(QStringLiteral("2000000000"), TokenUnits(0), true, true)->price, CAmount{1});
        QVERIFY(!ParseDisplayLimit(QStringLiteral("2000000000"), TokenUnits(0), true, false));
        QCOMPARE(ParseQuantity(QStringLiteral("0.000001"), units).value(), CAmount{1});
        QVERIFY(!ParseQuantity(QStringLiteral("1"), TokenUnits(18))); // amount exceeds MAX_MONEY
    }
    void displaySidesReverseWithoutChangingCanonicalRequests()
    {
        using namespace B3FlowMeshMarketData;
        for (bool inverse : {false, true}) {
            for (bool buy : {false, true}) {
                const auto side{CanonicalSide(buy, inverse)};
                QCOMPARE(side, buy != inverse ? QStringLiteral("bid") : QStringLiteral("ask"));
                QCOMPARE(DisplayBuy(side, inverse), buy);
            }
        }
        QVERIFY(Rejected([] { DisplayBuy(QStringLiteral("other"), true); }));
        auto action{Order()};
        const auto original{action.market};
        action.inverse_display = true;
        action.price = ParseDisplayPrice(QStringLiteral("2"), TokenUnits(), true).value();
        action.amount = 1'000'000; // The ticket still specifies token atoms.
        for (bool buy_b3 : {false, true}) {
            action.side = CanonicalSide(buy_b3, true);
            const auto request{Parameters(action)};
            QCOMPARE(request.method, std::string{"submitflowmeshorder"});
            QCOMPARE(request.params[0].get_str(), original.id.toStdString());
            QCOMPARE(request.params[1].get_str(), buy_b3 ? std::string{"ask"} : std::string{"bid"});
            QCOMPARE(request.params[2].getInt<int64_t>(), int64_t{500});
            QCOMPARE(request.params[3].getInt<int64_t>(), int64_t{1'000'000});
            QCOMPARE(request.params[4].getInt<int64_t>(), int64_t{7});
            QCOMPARE(action.market.base, original.base);
            QCOMPARE(action.market.vault, original.vault);
            CheckSameMarket(original, action.market, true);
            action.operation = Operation::Cancel;
            QCOMPARE(Parameters(action).params[1].get_str(), action.side.toStdString());
            action.operation = Operation::Order;
        }
        auto changed{original}; changed.base = QString::fromStdString(H(90).GetHex());
        QVERIFY(Rejected([&] { CheckSameMarket(original, changed, true); }));
        changed = original; changed.vault = QString::fromStdString(H(91).GetHex());
        QVERIFY(Rejected([&] { CheckSameMarket(original, changed, true); }));
        action.operation = Operation::Deposit;
        QCOMPARE(Parameters(action).params[1].get_str(), original.base.toStdString());
        action.native = true;
        QCOMPARE(Parameters(action).params[1].get_str(), std::string{"B3"});
    }
    void reverseAdapterPreservesCanonicalBytesDigestAndMicroblockExecution()
    {
        using namespace B3FlowMeshMarketData;
        for (bool buy_b3 : {false, true}) {
            auto reverse{Order()};
            reverse.inverse_display = true;
            reverse.display_decimals = 6;
            reverse.display_ticker = QStringLiteral("cUSD");
            reverse.price = ParseDisplayLimit(QStringLiteral("1.1"), TokenUnits(), true, buy_b3)->price;
            reverse.side = CanonicalSide(buy_b3, true);
            reverse.amount = ParseQuantity(QStringLiteral("3"), TokenUnits()).value();
            auto canonical{reverse}; canonical.inverse_display = false;
            canonical.display_decimals.reset(); canonical.display_ticker.clear();
            const auto rp{Parameters(reverse)}, cp{Parameters(canonical)};
            QCOMPARE(rp.method, cp.method);
            QCOMPARE(rp.params.write(), cp.params.write());
            auto ra{CanonicalOrder(reverse)}, ca{CanonicalOrder(canonical)};
            QCOMPARE(Encoded(ra), Encoded(ca));
            QVERIFY(ra.Id() == ca.Id());
            const auto domain{*uint256::FromHex(reverse.market.domain.toStdString())};
            const auto config{*uint256::FromHex(reverse.market.config.toStdString())};
            QVERIFY(flowmesh::ActionSignatureDigest(domain, config, ra) == flowmesh::ActionSignatureDigest(domain, config, ca));
            // Metadata never enters the canonical identity or digest. A
            // different AssetId, however, changes the execution config and
            // cannot be treated as the same market merely for sharing cUSD.
            const auto base{*uint256::FromHex(reverse.market.base.toStdString())};
            const auto vault{*uint256::FromHex(reverse.market.vault.toStdString())};
            flowmesh::FlowMeshState previous{vault, base, modern::NativeAsset()};
            flowmesh::FlowMeshState wrong_asset{vault, H(90), modern::NativeAsset()};
            QVERIFY(previous.ConfigId() != wrong_asset.ConfigId());
            QVERIFY(flowmesh::ActionSignatureDigest(domain, previous.ConfigId(), ra) != flowmesh::ActionSignatureDigest(domain, wrong_asset.ConfigId(), ra));
            QVERIFY(flowmesh::test_only::StateFunding::Fund(previous, ra.signer, base, 10'000'000));
            QVERIFY(flowmesh::test_only::StateFunding::Fund(previous, ra.signer, modern::NativeAsset(), 10 * KILO_COIN));
            flowmesh::test_only::StateFunding::SetNextSequence(previous, ra.signer, ra.sequence);
            flowmesh::FlowMeshState rn{previous}, cn{previous};
            flowmesh::BatchResult rr, cr;
            std::vector<std::vector<unsigned char>> re, ce;
            const flowmesh::AnchorRef anchor{20, H(20)};
            const auto rm{flowmesh::BuildMicroblock(previous, domain, {}, anchor, {ra}, nullptr, rn, rr, re)};
            const auto cm{flowmesh::BuildMicroblock(previous, domain, {}, anchor, {ca}, nullptr, cn, cr, ce)};
            QVERIFY(rm && cm);
            QCOMPARE(rr.applied.size(), size_t{1}); QVERIFY(rr.rejected.empty());
            QCOMPARE(Encoded(*rm), Encoded(*cm));
            QVERIFY(rm->GetHash() == cm->GetHash());
            QVERIFY(rn.Root() == cn.Root()); QVERIFY(rr.result_commitment == cr.result_commitment);
            QCOMPARE(Encoded(rn), Encoded(cn));
            QVERIFY(rn.ConfigId() == previous.ConfigId());
            // Independently execute the unchanged V1 core as an existing
            // peer does. This does not claim a live signature/certificate.
            flowmesh::FlowMeshState verified{previous}; flowmesh::BatchResult vr;
            QCOMPARE(flowmesh::ExecuteCandidate(previous, domain, {}, *rm, nullptr, verified, vr), flowmesh::CandidateError::NONE);
            QVERIFY(verified.Root() == rn.Root());
        }
    }
    void inverseBuyPartialBetterFillAndCancelUseActualV1Fees()
    {
        using namespace B3FlowMeshMarketData;
        const auto base{H(2)}, user{H(10)}, another_seller{H(11)}, buyer{H(12)};
        const auto& b3{modern::NativeAsset()};
        flowmesh::FlowMeshState state{H(1), base, b3};
        const auto& ledger{state.LedgerView()};
        QVERIFY(flowmesh::test_only::StateFunding::Fund(state, user, base, 3'000'000));
        QVERIFY(flowmesh::test_only::StateFunding::Fund(state, another_seller, base, 9'000'000));
        QVERIFY(flowmesh::test_only::StateFunding::Fund(state, buyer, b3, 2'400'000'000));
        const auto limit{ParseDisplayLimit(QStringLiteral("2"), TokenUnits(), true, true)};
        QVERIFY(limit); QCOMPARE(limit->price, CAmount{500});
        using Side = flowmesh::ClearingEngine::Side;
        QVERIFY(state.SubmitCurve(user, Side::ASK, *flowmesh::MakeLimitAskCurve(limit->price, 3'000'000)));
        QCOMPARE(ledger.Reserved(user, base), CAmount{3'000'000});
        QCOMPARE(ledger.Reserved(user, b3), CAmount{0});
        // Competing liquidity selects 600, and rationing fills exactly one
        // of this customer's three token units: a real partial better fill.
        QVERIFY(state.SubmitCurve(another_seller, Side::ASK, *flowmesh::MakeLimitAskCurve(600, 9'000'000)));
        QVERIFY(state.SubmitCurve(buyer, Side::BID, *flowmesh::MakeLimitBidCurve(600, 4'000'000)));
        const auto fees{FeeContext()};
        const auto result{state.ClearSlot(&fees)};
        QVERIFY(result && result->cleared);
        QCOMPARE(result->price, CAmount{600});
        QCOMPARE(result->ask_fill.at(user), CAmount{1'000'000});
        QCOMPARE(ledger.Available(user, b3), CAmount{599'940'000});
        QCOMPARE(ledger.Reserved(user, base), CAmount{2'000'000});
        QCOMPARE(result->fees.fee_total, CAmount{240'000});
        QCOMPARE(result->fees.treasury_fee, CAmount{48'000});
        QCOMPARE(result->fees.seat_fee, CAmount{192'000});
        QCOMPARE(ledger.Available(flowmesh::FlowMeshTreasuryFeeAccount(fees), b3), CAmount{48'000});
        CAmount rewards{0};
        for (const auto& seat : fees.seats) {
            rewards += ledger.Available(flowmesh::FlowMeshSeatRewardAccount(fees, seat), b3);
            QCOMPARE(ledger.Available(flowmesh::FlowMeshSeatRewardAccount(fees, seat), base), CAmount{0});
        }
        QCOMPARE(rewards, CAmount{192'000});
        QVERIFY(state.CancelCurve(user, Side::ASK));
        QCOMPARE(ledger.Reserved(user, base), CAmount{0});
        QCOMPARE(ledger.Available(user, base), CAmount{2'000'000});
        QCOMPARE(ledger.Available(user, b3), CAmount{599'940'000});
        QVERIFY(ledger.SolvencyHolds());
        // A one-B3 gross target at the limit is not a net/fixed guarantee.
        QCOMPARE(Notional(500, 2'000'000).value(), CAmount{KILO_COIN});
        QCOMPARE(Notional(600, 2'000'000).value(), CAmount{1'200'000'000});
        QCOMPARE(FeeExample(KILO_COIN).value(), CAmount{100'000});
        QVERIFY(KILO_COIN - *FeeExample(KILO_COIN) < KILO_COIN);
        // Exact all-in price is 20000/9999 token units per net B3, above
        // the displayed gross cap of 2. The UI must call the cap pre-fee.
        using Wide = util::Unsigned128;
        QVERIFY(Wide{2'000'000} * KILO_COIN * 9999 ==
                Wide{1'000'000} * (KILO_COIN - *FeeExample(KILO_COIN)) * 20000);
    }
    void marketableLimitClearsAgainstTheBookItWasPricedFrom()
    {
        using namespace B3FlowMeshMarketData;
        using Side = flowmesh::ClearingEngine::Side;
        using flowmesh::test_only::StateFunding;
        const auto base{H(2)}, user{H(10)};
        const auto& b3{modern::NativeAsset()};
        const auto fees{FeeContext()};
        struct Resting { unsigned char owner; bool bid; CAmount price, quantity; };
        const std::vector<Resting> resting{{20, true, 1000, 100'000}, {21, true, 998, 120'000}, {22, true, 960, 500'000},
                                           {80, false, 1010, 150'000}, {81, false, 1012, 170'000}, {82, false, 1050, 400'000}};
        for (const bool buy : {true, false}) {
            flowmesh::FlowMeshState state{H(1), base, b3};
            // The certified book the helper sees is exactly the executing state.
            B3FlowMeshMarketData::Snapshot s; s.certified = true; s.curves_complete = true; s.base = QString::fromStdString(base.GetHex()); s.units = TokenUnits();
            for (const auto& r : resting) {
                const auto account{H(r.owner)};
                QVERIFY(StateFunding::Fund(state, account, r.bid ? b3 : base, r.bid ? r.price * r.quantity : r.quantity));
                const auto points{r.bid ? flowmesh::MakeLimitBidCurve(r.price, r.quantity) : flowmesh::MakeLimitAskCurve(r.price, r.quantity)};
                QVERIFY(points && state.SubmitCurve(account, r.bid ? Side::BID : Side::ASK, *points));
                Curve curve; curve.account = QString::fromStdString(account.GetHex()); curve.side = r.bid ? QStringLiteral("bid") : QStringLiteral("ask");
                for (const auto& point : *points) curve.points.push_back({point.price, point.qty});
                curve.remaining = r.quantity;
                s.curves.push_back(curve);
            }
            const CAmount quantity{200'000};
            QString error;
            const auto limit{ComputeMarketableLimit(s, buy, false, 100, quantity, &error)};
            QVERIFY2(limit, qPrintable(error));
            QCOMPARE(limit->price, buy ? CAmount{1020} : CAmount{990});
            QVERIFY(StateFunding::Fund(state, user, buy ? b3 : base, buy ? limit->price * quantity : quantity));
            const auto own{buy ? flowmesh::MakeLimitBidCurve(limit->price, quantity) : flowmesh::MakeLimitAskCurve(limit->price, quantity)};
            QVERIFY(own && state.SubmitCurve(user, buy ? Side::BID : Side::ASK, *own));
            const auto result{state.ClearSlot(&fees)};
            QVERIFY(result && result->cleared);
            // Only inequalities: tie-breaking, rationing and other demand in
            // the same batch decide the exact fill.
            QVERIFY(buy ? result->price <= limit->price : result->price >= limit->price);
            const auto& fills{buy ? result->bid_fill : result->ask_fill};
            const CAmount filled{fills.contains(user) ? fills.at(user) : 0};
            QVERIFY(filled <= std::min(quantity, limit->visible));
            QVERIFY(filled > 0);
        }
    }
    void inverseSellPartialBetterFillKeepsSavingsUntilCancel()
    {
        using namespace B3FlowMeshMarketData;
        const auto base{H(2)}, user{H(10)}, seller{H(11)};
        const auto& b3{modern::NativeAsset()};
        flowmesh::FlowMeshState state{H(1), base, b3};
        const auto& ledger{state.LedgerView()};
        QVERIFY(flowmesh::test_only::StateFunding::Fund(state, user, b3, 1'500'000'000));
        QVERIFY(flowmesh::test_only::StateFunding::Fund(state, seller, base, 1'000'000));
        const auto limit{ParseDisplayLimit(QStringLiteral("2"), TokenUnits(), true, false)};
        QVERIFY(limit); QCOMPARE(limit->price, CAmount{500});
        using Side = flowmesh::ClearingEngine::Side;
        QVERIFY(state.SubmitCurve(user, Side::BID, *flowmesh::MakeLimitBidCurve(limit->price, 3'000'000)));
        QCOMPARE(ledger.Reserved(user, b3), CAmount{1'500'000'000});
        QCOMPARE(ledger.Reserved(user, base), CAmount{0});
        QVERIFY(state.SubmitCurve(seller, Side::ASK, *flowmesh::MakeLimitAskCurve(400, 1'000'000)));
        const auto fees{FeeContext()};
        const auto result{state.ClearSlot(&fees)};
        QVERIFY(result && result->cleared);
        QCOMPARE(result->price, CAmount{400});
        QCOMPARE(result->bid_fill.at(user), CAmount{1'000'000});
        QCOMPARE(ledger.Available(user, base), CAmount{1'000'000});
        QCOMPARE(ledger.Reserved(user, b3), CAmount{1'100'000'000});
        QCOMPARE(ledger.Available(user, b3), CAmount{0});
        QCOMPARE(result->fees.fee_total, CAmount{40'000});
        QCOMPARE(result->fees.seller_fees.size(), size_t{1});
        QVERIFY(result->fees.seller_fees[0].account == seller);
        QCOMPARE(ledger.Available(seller, b3), CAmount{399'960'000});
        QVERIFY(state.CancelCurve(user, Side::BID));
        QCOMPARE(ledger.Reserved(user, b3), CAmount{0});
        QCOMPARE(ledger.Available(user, b3), CAmount{1'100'000'000});
        QCOMPARE(ledger.Available(user, base), CAmount{1'000'000});
        QVERIFY(ledger.SolvencyHolds());
        QCOMPARE(Notional(400, 2'000'000).value(), CAmount{800'000'000});
        // A separately submitted remaining two-token target then exhausts
        // at the improved price. The engine automatically releases its
        // unused B3 reservation; no display-derived release is fabricated.
        QVERIFY(flowmesh::test_only::StateFunding::Fund(state, seller, base, 2'000'000));
        QVERIFY(state.SubmitCurve(user, Side::BID, *flowmesh::MakeLimitBidCurve(500, 2'000'000)));
        QVERIFY(state.SubmitCurve(seller, Side::ASK, *flowmesh::MakeLimitAskCurve(400, 2'000'000)));
        const auto completed{state.ClearSlot(&fees)};
        QVERIFY(completed && completed->cleared);
        QCOMPARE(ledger.Reserved(user, b3), CAmount{0});
        QCOMPARE(ledger.Available(user, b3), CAmount{300'000'000});
        QCOMPARE(ledger.Available(user, base), CAmount{3'000'000});
        QVERIFY(ledger.SolvencyHolds());
    }
    void tinyQuantitiesAndLargestRemainderFeesAreNotDisplayEstimates()
    {
        using namespace B3FlowMeshMarketData;
        const auto base{H(2)}, buyer{H(10)}, seller{H(11)};
        const auto& b3{modern::NativeAsset()};
        flowmesh::FlowMeshState state{H(1), base, b3};
        QVERIFY(flowmesh::test_only::StateFunding::Fund(state, buyer, b3, 1));
        QVERIFY(flowmesh::test_only::StateFunding::Fund(state, seller, base, 1));
        using Side = flowmesh::ClearingEngine::Side;
        QVERIFY(state.SubmitCurve(buyer, Side::BID, *flowmesh::MakeLimitBidCurve(1, 1)));
        QVERIFY(state.SubmitCurve(seller, Side::ASK, *flowmesh::MakeLimitAskCurve(1, 1)));
        const auto fees{FeeContext()};
        const auto result{state.ClearSlot(&fees)};
        QVERIFY(result && result->cleared);
        QCOMPARE(result->fees.fee_total, CAmount{0});
        QCOMPARE(state.LedgerView().Available(buyer, base), CAmount{1});
        QCOMPARE(state.LedgerView().Available(seller, b3), CAmount{1});
        QCOMPARE(FormatAmount(1, 6), QStringLiteral("0.000001"));
        QCOMPARE(FormatAmount(1, 18), QStringLiteral("0.000000000000000001"));
        std::vector<flowmesh::SeatId> seats;
        for (const auto& seat : fees.seats) seats.push_back(seat.seat_id);
        const std::vector<flowmesh::SellerQuoteProceeds> proceeds{{H(10), 5001}, {H(11), 5000}};
        flowmesh::FeeAllocationCheck check;
        const auto allocated{flowmesh::AllocateFlowMeshFees(10'001, proceeds, seats, check)};
        QVERIFY(allocated); QCOMPARE(check, flowmesh::FeeAllocationCheck::OK);
        QCOMPARE(allocated->fee_total, CAmount{1});
        QCOMPARE(allocated->seller_fees[0].fee, CAmount{1});
        QCOMPARE(allocated->seller_fees[1].fee, CAmount{0});
        QCOMPARE(FeeExample(5001).value(), CAmount{0});
        QCOMPARE(allocated->treasury_fee, CAmount{0});
        QCOMPARE(allocated->seat_fee, CAmount{1});
        QCOMPARE(allocated->seat_rewards[0].reward, CAmount{1});
        // Independent per-customer floor estimates cannot stand in for the
        // actual certified largest-remainder allocation, even at one atom.
        QVERIFY(allocated->seller_fees[0].fee != FeeExample(5001).value());
    }
    void inverseReviewKeepsExactTokenQuantityAndFeesInB3()
    {
        auto action{Order()};
        action.inverse_display = true;
        action.display_decimals = 6;
        action.display_ticker = QStringLiteral("cUSD");
        action.price = 500;
        action.side = QStringLiteral("ask");
        auto review{Describe(action)};
        QVERIFY(review.contains(QStringLiteral("Buy B3: Spend up to 1 cUSD (1000000 atomic units)")));
        QVERIFY(review.contains(QStringLiteral("2 cUSD / B3")));
        QVERIFY(review.contains(QStringLiteral("Estimated gross B3 at the limit: 0.50 B3")));
        QVERIFY(review.contains(QStringLiteral("NOT B3")));
        QVERIFY(review.contains(QStringLiteral("deducted from the B3 you receive")));
        QVERIFY(review.contains(QStringLiteral("Canonical side: ask")));
        QVERIFY(review.contains(action.market.base));
        action.side = QStringLiteral("bid");
        review = Describe(action);
        QVERIFY(review.contains(QStringLiteral("Sell B3: Receive up to 1 cUSD (1000000 atomic units)")));
        QVERIFY(review.contains(QStringLiteral("not deducted from your token receipt")));
        QVERIFY(review.contains(QStringLiteral("Canonical side: bid")));
        // A nonterminating reciprocal is disclosed exactly in the review.
        action.price = 901;
        QVERIFY(Describe(action).contains(QStringLiteral("1000/901 cUSD / B3")));
        action.display_decimals.reset();
        QVERIFY(Rejected([&] { Describe(action); }));
    }
    void replacementBudgetIsCheckedAndSpecificToTheCanonicalSide()
    {
        using namespace B3FlowMeshMarketData;
        QCOMPARE(ReplacementBudget(0, 0).value(), CAmount{0});
        QCOMPARE(ReplacementBudget(10, 20).value(), CAmount{30});
        QCOMPARE(ReplacementBudget(MAX_MONEY, 0).value(), MAX_MONEY);
        QCOMPARE(ReplacementBudget(MAX_MONEY - 1, 1).value(), MAX_MONEY);
        QVERIFY(!ReplacementBudget(MAX_MONEY, 1));
        QVERIFY(!ReplacementBudget(MAX_MONEY + 1, 0));
        QVERIFY(!ReplacementBudget(-1, 1));
        QVERIFY(!ReplacementBudget(1, -1));
        auto action{Order()};
        action.market.b3_available = 500'000'000;
        action.market.b3_reserved = 500'000'000;
        QCOMPARE(Parameters(action).params[3].getInt<int64_t>(), int64_t{1'000'000});
        ++action.amount;
        QVERIFY(Rejected([&] { Parameters(action); }));
        action.amount = 1'000'000;
        action.market.b3_reserved = 0;
        action.market.base_reserved = MAX_MONEY;
        QVERIFY(Rejected([&] { Parameters(action); })); // Token reserve cannot fund a bid.
        action.side = QStringLiteral("ask");
        action.market.base_available = 400'000;
        action.market.base_reserved = 600'000;
        QCOMPARE(Parameters(action).params[3].getInt<int64_t>(), int64_t{1'000'000});
        ++action.amount;
        QVERIFY(Rejected([&] { Parameters(action); }));
        action.amount = 1'000'000;
        action.market.base_reserved = 0;
        action.market.b3_reserved = MAX_MONEY;
        QVERIFY(Rejected([&] { Parameters(action); })); // B3 reserve cannot fund an ask.
        action.market.base_reserved = 600'000;
        action.operation = Operation::Withdraw;
        action.destination = Address();
        action.amount = action.market.base_available + 1;
        QVERIFY(Rejected([&] { Parameters(action); })); // Replacement credit is not withdrawable.
        action.native = true;
        action.amount = action.market.b3_available + 1;
        QVERIFY(Rejected([&] { Parameters(action); }));
    }
    void inverseDepthAmountsUseExactGrossB3AndWideProducts()
    {
        using namespace B3FlowMeshMarketData;
        QCOMPARE(FormatDepthQuantity(500, 250'000, 6, false), QStringLiteral("0.25"));
        QCOMPARE(FormatDepthQuantity(500, 250'000, 6, true), QStringLiteral("0.125"));
        QCOMPARE(FormatDepthQuantity(3, 1, 6, true), QStringLiteral("0.000000003"));
        QCOMPARE(FormatDepthQuantity(0, MAX_MONEY, 6, true), QStringLiteral("0"));
        QCOMPARE(FormatDepthQuantity(MAX_MONEY, 0, 6, true), QStringLiteral("0"));
        // Hypothetical curve depth is display-only and can exceed a single
        // spendable notional; the multiplication must not wrap int64_t.
        QCOMPARE(FormatDepthQuantity(1'000'000'000'000, 1'000'000'000'000, 6, true),
                 QStringLiteral("1000000000000000"));
        QCOMPARE(FormatDepthQuantity(MAX_MONEY, MAX_MONEY, 6, true),
                 QStringLiteral("438508840000000000000000000"));
        QCOMPARE(FormatDepthQuantity(-1, 1, 6, true), QStringLiteral("—"));
        QCOMPARE(FormatDepthQuantity(1, -1, 6, true), QStringLiteral("—"));
    }
    void nativeAndAssetUnitsRemainDistinct() {
        auto a{Order()}; a.operation = Operation::Deposit; a.amount = 1'000'001;
        auto r{Parameters(a)}; QCOMPARE(r.method, std::string{"flowmeshdeposit"}); QCOMPARE(r.params[2].getInt<int64_t>(), a.amount);
        QVERIFY(!r.params[3]["broadcast"].get_bool()); QCOMPARE(r.params[3]["minconf"].getInt<int>(), 1);
        QVERIFY(!r.params[3]["include_unsafe"].get_bool()); QVERIFY(r.params[3]["market_bootstrap"].isNull());
        a.native = true; r = Parameters(a); QCOMPARE(r.params[1].get_str(), std::string{"B3"});
        QCOMPARE(r.params[2].write(), std::string{"0.001000001"});
        a.operation = Operation::Withdraw; a.destination = Address(); r = Parameters(a);
        QCOMPARE(r.method, std::string{"requestflowmeshwithdrawal"}); QCOMPARE(r.params[2].write(), std::string{"0.001000001"});
        QCOMPARE(r.params[4].getInt<int>(), 7);
    }
    void pausedGenesisOnlyAllowsPublication() {
        auto a{Order()}; a.market = ParseMarket(Snapshot(true)); QVERIFY(!a.market.ready); QVERIFY(a.market.publish_ready);
        QVERIFY(Rejected([&] { Parameters(a); }));
        a.operation = Operation::Deposit; QVERIFY(Rejected([&] { Parameters(a); }));
        a.operation = Operation::Checkpoint; const auto r{Parameters(a)};
        QCOMPARE(r.method, std::string{"createflowmeshcheckpoint"}); QVERIFY(!r.params[1]["broadcast"].get_bool());
        CheckSameMarket(a.market, a.market, false, true);
        QVERIFY(Rejected([&] { CheckSameMarket(a.market, a.market, false); }));
    }
    void cancellationNeverCallsRpc() {
        auto a{Order()}; int calls{0}; const RpcCall rpc = [&](const auto&, const auto&) { ++calls; return UniValue{}; };
        QVERIFY(Rejected([&] { DispatchApproved(a, false, [] { return false; }, rpc); }));
        QVERIFY(Rejected([&] { DispatchApproved(a, true, [] { return true; }, rpc); })); QCOMPARE(calls, 0);
        bool cancelled{false};
        const RpcCall reads = [&](const std::string& method, const UniValue&) {
            ++calls; if (method == "listflowmeshmarkets") { UniValue rows{UniValue::VARR}; rows.push_back(Snapshot()); return rows; }
            if (method == "getflowmeshbalance") { cancelled = true; return Snapshot(); }
            throw std::runtime_error{"A cancelled action must never reach a write"};
        };
        QVERIFY(Rejected([&] { DispatchApproved(a, true, [&] { return cancelled; }, reads); })); QCOMPARE(calls, 2);
    }
    void changedSequenceAndCheckpointRequireNewReview() {
        auto a{Order()}; auto fresh{a.market}; ++fresh.sequence;
        QVERIFY(Rejected([&] { CheckSameMarket(a.market, fresh, true); }));
        a.operation = Operation::Checkpoint; int writes{0};
        const RpcCall rpc = [&](const std::string& method, const UniValue&) {
            UniValue v{Snapshot()}; v.pushKV("pending_checkpoint_id", H(9).GetHex());
            if (method == "listflowmeshmarkets") { UniValue rows{UniValue::VARR}; rows.push_back(v); return rows; }
            if (method == "getflowmeshbalance") return v;
            ++writes; return UniValue{};
        };
        QVERIFY(Rejected([&] { DispatchApproved(a, true, [] { return false; }, rpc); })); QCOMPARE(writes, 0);
    }
    void malformedIdentityAndSyncFailClosed() {
        auto v{Snapshot()}; v.pushKV("vault_id", H(99).GetHex()); QVERIFY(Rejected([&] { ParseMarket(v); }));
        UniValue chain{UniValue::VOBJ}; chain.pushKV("initialblockdownload", false); chain.pushKV("headers", 2); chain.pushKV("blocks", 1);
        QVERIFY(Rejected([&] { CheckSynced(chain); })); chain.pushKV("blocks", 2); CheckSynced(chain);
        chain.pushKV("initialblockdownload", true); QVERIFY(Rejected([&] { CheckSynced(chain); }));
    }
    void watchBackoffDoublesFromOneToThirtySecondsWithJitter()
    {
        using B3FlowMeshWatch::Policy; using B3FlowMeshWatch::Mode; using B3FlowMeshWatch::Classify; using WOutcome = B3FlowMeshWatch::Outcome;
        Policy p; p.Enable(0, true);
        const std::array<int64_t, 7> expected{1000, 2000, 4000, 8000, 16000, 30000, 30000};
        int64_t now{0};
        for (const auto backoff : expected) {
            QVERIFY(p.MayArm(now)); p.Armed(now);
            p.Finished(now, WOutcome::Failed, 0.0);
            QCOMPARE(p.mode, Mode::Backoff); QCOMPARE(p.BackoffMs(), backoff);
            QCOMPARE(p.RetryInMs(now), backoff);
            QVERIFY(!p.MayArm(now + backoff - 1)); QVERIFY(p.PollEveryTick());
            now += backoff;
        }
        // Jitter spreads a retry by at most a fifth either way.
        Policy low; low.Enable(0, true); QVERIFY(low.MayArm(0)); low.Armed(0); low.Finished(0, WOutcome::Failed, -0.2);
        QCOMPARE(low.retry_at, int64_t{800});
        Policy high; high.Enable(0, true); QVERIFY(high.MayArm(0)); high.Armed(0); high.Finished(0, WOutcome::Failed, 0.9);
        QCOMPARE(high.retry_at, int64_t{1200});
        // A waited reply clears the failures.
        QVERIFY(p.MayArm(now)); p.Armed(now); p.Finished(now, WOutcome::Quiet, 0.0);
        QCOMPARE(p.mode, Mode::Live); QCOMPARE(p.failures, 0U); QVERIFY(!p.PollEveryTick());
    }
    void watchUnsupportedAndBusyFallBackToPollingAndRetry()
    {
        using B3FlowMeshWatch::Policy; using B3FlowMeshWatch::Mode; using B3FlowMeshWatch::Classify; using WOutcome = B3FlowMeshWatch::Outcome;
        for (const auto& [outcome, retry] : {std::pair{WOutcome::Busy, Policy::BUSY_RETRY_MS}, std::pair{WOutcome::Unsupported, Policy::UNSUPPORTED_RETRY_MS}}) {
            Policy p; p.Enable(0, true); QVERIFY(p.MayArm(0)); p.Armed(0);
            p.Finished(100, outcome, 0.0);
            QCOMPARE(p.mode, Mode::Polling); QVERIFY(p.PollEveryTick());
            QVERIFY(!p.MayArm(100 + retry - 1));
            QVERIFY(p.MayArm(100 + retry)); QCOMPARE(p.mode, Mode::Probing);
        }
        // An endpoint that advertises nothing starts in Polling.
        Policy none; none.Enable(0, false);
        QCOMPARE(none.mode, Mode::Polling); QVERIFY(!none.MayArm(1000));
        // The watch object decides, keyed on the endpoint the watch was for.
        QCOMPARE(Classify(false, "not_eligible", false, false, false, 0), WOutcome::Unsupported);
        QCOMPARE(Classify(false, "not_eligible", false, false, false, 2000), WOutcome::NotWaited);
        QCOMPARE(Classify(false, "unsupported", false, false, false, 2000), WOutcome::Unsupported);
        QCOMPARE(Classify(false, "busy", true, false, false, 2000), WOutcome::Busy);
        QCOMPARE(Classify(true, "", false, false, false, 2000), WOutcome::Failed);
        QCOMPARE(Classify(false, "timeout", true, true, false, 2000), WOutcome::Quiet);
        QCOMPARE(Classify(false, "ready", true, true, true, 2000), WOutcome::Changed);
        QCOMPARE(Classify(false, "ready", true, true, false, 2000), WOutcome::Quiet);
        for (const char* label : {"lane_busy", "superseded", "deadline", "interrupted", "revalidate", "other"}) {
            QCOMPARE(Classify(false, label, false, false, false, 2000), WOutcome::NotWaited);
        }
    }
    void watchReceiptEvidenceIsAHeadThatMovedDuringTheWait()
    {
        using B3FlowMeshWatch::HeadMovedDuringWait;
        // Woken by the event of a new head while waiting at the known one.
        QVERIFY(HeadMovedDuringWait("ready", true, false, true));
        // Answered at once, without waiting, for an already stale known head.
        QVERIFY(!HeadMovedDuringWait("head_changed", true, false, true));
        // Woken by another event (the head did not move), a gap, unsupported.
        QVERIFY(!HeadMovedDuringWait("ready", true, false, false));
        QVERIFY(!HeadMovedDuringWait("ready", true, true, true));
        QVERIFY(!HeadMovedDuringWait("ready", false, false, true));
        for (const char* label : {"timeout", "gap", "restarted", "ticks_catchup", "not_eligible", "lane_busy", "superseded",
                                  "revalidate", "busy", "deadline", "interrupted", "other", "none"}) {
            QVERIFY2(!HeadMovedDuringWait(label, true, false, true), label);
        }
    }
    void watchNonWaitedOutcomesArePaced()
    {
        using B3FlowMeshWatch::Policy; using B3FlowMeshWatch::Mode; using B3FlowMeshWatch::Classify; using WOutcome = B3FlowMeshWatch::Outcome;
        Policy p; p.Enable(0, true);
        int64_t now{0};
        unsigned arms{0};
        // Answered at once, again and again: never re-armed at once, and
        // three in a row fall back to polling for ten seconds.
        for (int i{0}; i < 3; ++i) {
            while (!p.MayArm(now)) now += 10;
            p.Armed(now); ++arms;
            p.Finished(now, WOutcome::NotWaited, 0.0);
            if (i < 2) { QVERIFY(!p.MayArm(now)); QVERIFY(!p.MayArm(now + Policy::TICK_MS - 1)); }
        }
        QCOMPARE(p.mode, Mode::Polling);
        QCOMPARE(p.RetryInMs(now), Policy::NOT_WAITED_RETRY_MS);
        QCOMPARE(arms, 3U);
        QVERIFY(now >= 2 * Policy::TICK_MS);
    }
    void watchChangedRepliesArePacedTwoPerSecond()
    {
        using B3FlowMeshWatch::Policy; using B3FlowMeshWatch::Mode; using B3FlowMeshWatch::Classify; using WOutcome = B3FlowMeshWatch::Outcome;
        Policy p; p.Enable(0, true);
        // A busy market: every read returns a changed head 10 ms later.
        std::vector<int64_t> arms;
        for (int64_t now{0}; now < 10'000; now += 10) {
            if (p.outstanding) { p.Finished(now, WOutcome::Changed, 0.0); continue; }
            if (p.MayArm(now)) { p.Armed(now); arms.push_back(now); }
        }
        // At most the burst plus two per second, whatever the outcome.
        QVERIFY(arms.size() <= size_t{2 + 2 * 10});
        QVERIFY(arms.size() >= size_t{2 * 10});
        for (size_t i{2}; i < arms.size(); ++i) QVERIFY(arms[i] - arms[i - 2] >= Policy::ARM_REFILL_MS);
        QCOMPARE(p.mode, Mode::Live); QVERIFY(!p.PollEveryTick());
    }
    void watchLocalEngineIsPacedAtTwoPerSecondLikeRemote()
    {
        using B3FlowMeshWatch::Policy; using B3FlowMeshWatch::Mode; using WOutcome = B3FlowMeshWatch::Outcome;
        // A busy market: every read returns a changed head 10 ms later.
        const auto run = [](Policy& p, int64_t until, int64_t from = 0) {
            std::vector<int64_t> arms;
            for (int64_t now{from}; now < until; now += 5) {
                if (p.outstanding) { if (now - p.armed_at >= 10) p.Finished(now, WOutcome::Changed, 0.0); continue; }
                if (p.MayArm(now)) { p.Armed(now); arms.push_back(now); }
            }
            return arms;
        };
        // The local engine is held at the remote pacing until its reads stop
        // contending with the FlowMesh worker for the node's market mutex
        // (review of 2026-09-30, C7): at most a burst of two, then two a second.
        QCOMPARE(Policy::LOCAL_ARM_REFILL_MS, Policy::ARM_REFILL_MS);
        Policy local; local.Enable(0, true, /*local=*/true);
        QVERIFY(local.local); QCOMPARE(local.RefillMs(), Policy::ARM_REFILL_MS);
        const auto paced{run(local, 10'000)};
        QVERIFY(paced.size() <= size_t{2 + 2 * 10});
        QVERIFY(paced.size() >= size_t{2 * 10});
        for (size_t i{2}; i < paced.size(); ++i) QVERIFY(paced[i] - paced[i - 2] >= Policy::ARM_REFILL_MS);
        QCOMPARE(local.mode, Mode::Live);
        // Remote: unchanged, two per second.
        Policy remote; remote.Enable(0, true);
        QVERIFY(!remote.local); QCOMPARE(remote.RefillMs(), Policy::ARM_REFILL_MS);
        const auto slow{run(remote, 10'000)};
        QVERIFY(slow.size() <= size_t{2 + 2 * 10});
        for (size_t i{2}; i < slow.size(); ++i) QVERIFY(slow[i] - slow[i - 2] >= Policy::ARM_REFILL_MS);
        // A reply that names an endpoint: the remote pacing at once.
        local.Remote();
        QVERIFY(!local.local); QCOMPARE(local.RefillMs(), Policy::ARM_REFILL_MS);
        const auto after{run(local, 20'000, 10'000)};
        QVERIFY(after.size() <= size_t{2 + 2 * 10});
        for (size_t i{2}; i < after.size(); ++i) QVERIFY(after[i] - after[i - 2] >= Policy::ARM_REFILL_MS);
        // Re-enabling as remote keeps it remote.
        local.Enable(20'000, true);
        QCOMPARE(local.RefillMs(), Policy::ARM_REFILL_MS);
    }
    void watchNextArmInIsTheEarliestMayArm()
    {
        using B3FlowMeshWatch::Policy; using B3FlowMeshWatch::Mode; using WOutcome = B3FlowMeshWatch::Outcome;
        // Random outcome sequences at random times: NextArmIn names exactly
        // the first millisecond MayArm allows (checked on copies: MayArm
        // turns a due Polling or Backoff retry into Probing).
        uint64_t state{0x9e3779b97f4a7c15ULL};
        const auto next = [&](uint64_t bound) { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state % bound; };
        constexpr std::array outcomes{WOutcome::Changed, WOutcome::Quiet, WOutcome::NotWaited, WOutcome::Busy,
                                      WOutcome::Unsupported, WOutcome::Failed, WOutcome::Refreshed};
        for (int run{0}; run < 200; ++run) {
            Policy p; p.Enable(0, next(4) != 0);
            int64_t now{0};
            for (int step{0}; step < 40; ++step) {
                const auto wait{p.NextArmIn(now)};
                QVERIFY(wait >= 0);
                for (const int64_t before : {int64_t{0}, wait / 2, wait - 1}) {
                    if (before < 0 || before >= wait) continue;
                    Policy early{p}; QVERIFY(!early.MayArm(now + before));
                }
                { Policy due{p}; QVERIFY(due.MayArm(now + wait)); }
                now += wait + static_cast<int64_t>(next(3) == 0 ? next(700) : 0);
                QVERIFY(p.MayArm(now)); p.Armed(now);
                QCOMPARE(p.NextArmIn(now), int64_t{-1}); // Outstanding.
                now += static_cast<int64_t>(next(1600));
                p.Finished(now, outcomes[next(outcomes.size())], static_cast<double>(next(401)) / 1000.0 - 0.2);
            }
        }
        Policy off; QCOMPARE(off.NextArmIn(0), int64_t{-1});
        // A burst of two, then one per 500 ms.
        Policy p; p.Enable(0, true);
        QCOMPARE(p.NextArmIn(0), int64_t{0}); p.MayArm(0); p.Armed(0); p.Finished(10, WOutcome::Changed, 0.0);
        QCOMPARE(p.NextArmIn(10), int64_t{0}); p.MayArm(10); p.Armed(10); p.Finished(20, WOutcome::Changed, 0.0);
        QCOMPARE(p.NextArmIn(20), Policy::ARM_REFILL_MS - 20);
        // Answered without waiting: never before the retry time.
        p.MayArm(Policy::ARM_REFILL_MS); p.Armed(Policy::ARM_REFILL_MS); p.Finished(Policy::ARM_REFILL_MS + 5, WOutcome::NotWaited, 0.0);
        QCOMPARE(p.NextArmIn(Policy::ARM_REFILL_MS + 5), Policy::TICK_MS);
    }
    void watchProbeHoldsTheMarketPollForABoundedTime()
    {
        using B3FlowMeshWatch::Policy; using B3FlowMeshWatch::Mode; using WOutcome = B3FlowMeshWatch::Outcome;
        Policy p; p.Enable(0, true);
        // Nothing outstanding: probing keeps the ordinary poll.
        QVERIFY(!p.SuppliesMarketRead(0));
        QVERIFY(p.MayArm(0)); p.Armed(0);
        QVERIFY(p.ProbeHoldsPoll(0)); QVERIFY(p.SuppliesMarketRead(Policy::PROBE_HOLD_MS - 1));
        // A probe the endpoint holds on to never freezes the display longer.
        QVERIFY(!p.SuppliesMarketRead(Policy::PROBE_HOLD_MS));
        p.Finished(1000, WOutcome::NotWaited, 0.0);
        QCOMPARE(p.mode, Mode::Probing); QVERIFY(!p.SuppliesMarketRead(1000));
        QVERIFY(p.MayArm(1000 + Policy::TICK_MS)); p.Armed(1000 + Policy::TICK_MS);
        QVERIFY(p.SuppliesMarketRead(1000 + Policy::TICK_MS));
        p.Finished(2000, WOutcome::Quiet, 0.0);
        QCOMPARE(p.mode, Mode::Live); QVERIFY(!p.ProbeHoldsPoll(2000)); QVERIFY(p.SuppliesMarketRead(2000));
        QVERIFY(p.MayArm(2000)); p.Armed(2000); p.Finished(2000, WOutcome::Failed, 0.0);
        QCOMPARE(p.mode, Mode::Backoff); QVERIFY(!p.SuppliesMarketRead(2000));
    }
    void watchTickCatchupIsNeitherALiveReplyNorAStrike()
    {
        using B3FlowMeshWatch::Policy; using B3FlowMeshWatch::Mode; using B3FlowMeshWatch::Classify; using WOutcome = B3FlowMeshWatch::Outcome;
        QCOMPARE(Classify(false, "ticks_catchup", false, true, true, 2000), WOutcome::Refreshed);
        QCOMPARE(Classify(false, "ticks_catchup", false, true, false, 2000), WOutcome::Refreshed);
        QCOMPARE(Classify(true, "ticks_catchup", false, true, false, 2000), WOutcome::Failed);
        // Probing: still probing, no strike, re-armed at once.
        Policy p; p.Enable(0, true);
        QVERIFY(p.MayArm(0)); p.Armed(0); p.Finished(100, WOutcome::NotWaited, 0.0);
        QVERIFY(p.MayArm(100 + Policy::TICK_MS)); p.Armed(100 + Policy::TICK_MS);
        p.Finished(700, WOutcome::Refreshed, 0.0);
        QCOMPARE(p.mode, Mode::Probing); QCOMPARE(p.not_waited, 1U); QCOMPARE(p.RetryInMs(700), int64_t{0});
        QVERIFY(p.MayArm(1100)); p.Armed(1100);
        // Live: stays live without renewing the watchdog.
        p.Finished(1200, WOutcome::Quiet, 0.0);
        QCOMPARE(p.mode, Mode::Live); QCOMPARE(p.last_ok_at, int64_t{1200});
        QVERIFY(p.MayArm(1600)); p.Armed(1600); p.Finished(1700, WOutcome::Refreshed, 0.0);
        QCOMPARE(p.mode, Mode::Live); QCOMPARE(p.last_ok_at, int64_t{1200}); QCOMPARE(p.RetryInMs(1700), int64_t{0});
    }
    void watchQuietRepliesRearmImmediately()
    {
        using B3FlowMeshWatch::Policy; using B3FlowMeshWatch::Mode; using B3FlowMeshWatch::Classify; using WOutcome = B3FlowMeshWatch::Outcome;
        Policy p; p.Enable(0, true);
        QVERIFY(p.MayArm(0)); p.Armed(0);
        p.Finished(1500, WOutcome::Quiet, 0.0);
        QCOMPARE(p.mode, Mode::Live);
        QVERIFY(p.MayArm(1500)); p.Armed(1500);
        QVERIFY(!p.MayArm(1500)); // One outstanding at a time.
        p.Finished(3000, WOutcome::Quiet, 0.0);
        QVERIFY(p.MayArm(3000));
        // A discarded result records nothing but frees the slot.
        p.Armed(3000); p.Dropped(); QVERIFY(!p.outstanding); QCOMPARE(p.mode, Mode::Live);
        p.Disable(); QCOMPARE(p.mode, Mode::Off); QVERIFY(!p.MayArm(4000));
    }
    void watchdogTripsAfterFifteenSecondsOfSilence()
    {
        using B3FlowMeshWatch::Policy; using B3FlowMeshWatch::Mode; using B3FlowMeshWatch::Classify; using WOutcome = B3FlowMeshWatch::Outcome;
        Policy p; p.Enable(0, true);
        QVERIFY(p.MayArm(0)); p.Armed(0);
        QVERIFY(!p.WatchdogTripped(Policy::WATCHDOG_MS - 1));
        QVERIFY(p.WatchdogTripped(Policy::WATCHDOG_MS));
        p.Watchdog(Policy::WATCHDOG_MS);
        QCOMPARE(p.mode, Mode::Backoff); QVERIFY(p.PollEveryTick());
        p.Watchdog(Policy::WATCHDOG_MS + 500); QCOMPARE(p.failures, 1U); // Once per episode.
        QVERIFY(!p.MayArm(Policy::WATCHDOG_MS + 5000)); // Still outstanding.
        p.Finished(Policy::WATCHDOG_MS + 6000, WOutcome::Quiet, 0.0);
        QCOMPARE(p.mode, Mode::Live); QVERIFY(!p.WatchdogTripped(Policy::WATCHDOG_MS + 6000));
        // Live without a waited reply for fifteen seconds trips it too.
        p.Finished(Policy::WATCHDOG_MS + 6000, WOutcome::NotWaited, 0.0);
        QVERIFY(p.WatchdogTripped(2 * Policy::WATCHDOG_MS + 6000));
    }
    void linkStateClassification_data()
    {
        using B3FlowMeshMarketData::LinkState;
        QTest::addColumn<bool>("wallet");
        QTest::addColumn<bool>("failed");
        QTest::addColumn<qint64>("response");
        QTest::addColumn<qint64>("polling");
        QTest::addColumn<LinkState>("expected");
        QTest::newRow("no wallet") << false << false << qint64{100} << qint64{100'000} << LinkState::None;
        QTest::newRow("hidden") << true << false << qint64{100} << qint64{-1} << LinkState::Checking;
        QTest::newRow("fresh") << true << false << qint64{0} << qint64{100'000} << LinkState::Live;
        QTest::newRow("fresh edge") << true << false << qint64{3000} << qint64{100'000} << LinkState::Live;
        // Exactly the gate: a failed read is never Live, however fresh.
        QTest::newRow("fresh but failed") << true << true << qint64{100} << qint64{100'000} << LinkState::Reconnecting;
        QTest::newRow("slow") << true << false << qint64{3001} << qint64{100'000} << LinkState::Slow;
        QTest::newRow("slow edge") << true << false << qint64{30'000} << qint64{100'000} << LinkState::Slow;
        QTest::newRow("stale") << true << false << qint64{30'001} << qint64{100'000} << LinkState::Stale;
        QTest::newRow("failed") << true << true << qint64{10'000} << qint64{100'000} << LinkState::Reconnecting;
        QTest::newRow("failed and stale") << true << true << qint64{60'000} << qint64{100'000} << LinkState::Stale;
        QTest::newRow("never read, just shown") << true << false << qint64{-1} << qint64{500} << LinkState::Checking;
        QTest::newRow("never read, waiting") << true << false << qint64{-1} << qint64{5000} << LinkState::Slow;
        QTest::newRow("never read, failing") << true << true << qint64{-1} << qint64{5000} << LinkState::Reconnecting;
        QTest::newRow("never read for long") << true << false << qint64{-1} << qint64{31'000} << LinkState::Stale;
        // A reply from before the panel was hidden: only the time since it
        // was shown again counts, never the hidden interval.
        QTest::newRow("shown after an hour") << true << false << qint64{3'600'000} << qint64{200} << LinkState::Checking;
        QTest::newRow("shown, still waiting") << true << false << qint64{3'600'000} << qint64{4000} << LinkState::Slow;
        QTest::newRow("shown, failing") << true << true << qint64{3'600'000} << qint64{2000} << LinkState::Reconnecting;
        QTest::newRow("shown, failing long") << true << true << qint64{3'600'000} << qint64{30'001} << LinkState::Stale;
    }
    void linkStateClassification()
    {
        using B3FlowMeshMarketData::LinkState;
        QFETCH(bool, wallet); QFETCH(bool, failed); QFETCH(qint64, response); QFETCH(qint64, polling); QFETCH(LinkState, expected);
        B3FlowMeshMarketData::LinkInputs inputs;
        inputs.wallet = wallet; inputs.read_failed = failed; inputs.response_age_ms = response; inputs.polling_ms = polling;
        QCOMPARE(B3FlowMeshMarketData::ClassifyLink(inputs), expected);
        // Live is exactly the trading gate's freshness condition.
        QCOMPARE(expected == LinkState::Live, wallet && polling >= 0 && !failed && response >= 0 && response <= B3FlowMeshMarketData::LINK_FRESH_MS);
    }
    void sameHeadObservedTimesMergeWithoutChange()
    {
        using B3FlowMeshMarketData::ObservedTimes;
        const auto stamp = [](uint64_t sequence, int64_t ms) {
            flowmesh::ObservedStamp row; row.sequence = sequence; row.hash = uint256{static_cast<uint8_t>(sequence)};
            row.mode = flowmesh::ObservedMode::LIVE; row.ms = ms; return row;
        };
        ObservedTimes head_reply; head_reply.available = true; head_reply.source = QStringLiteral("https://operator.invalid:18443");
        head_reply.rows = {stamp(5, 1000), stamp(6, 1400), stamp(7, 1900)};
        // A same-head reply: the head's row only (a subset). Nothing changes.
        std::optional<ObservedTimes> retained{head_reply};
        auto head_only{head_reply}; head_only.rows = {stamp(7, 1900)};
        QVERIFY(!B3FlowMeshMarketData::MergeSameHeadObserved(retained, head_only));
        QCOMPARE(*retained, head_reply);
        // A row it changes (a flag the source added) or adds, in order.
        auto flagged{head_only}; flagged.rows[0].flags = 1;
        QVERIFY(B3FlowMeshMarketData::MergeSameHeadObserved(retained, flagged));
        QCOMPARE(retained->rows.size(), size_t{3}); QCOMPARE(retained->rows[2].flags, uint8_t{1});
        auto added{head_only}; added.rows = {stamp(4, 800)};
        QVERIFY(B3FlowMeshMarketData::MergeSameHeadObserved(retained, added));
        QCOMPARE(retained->rows.size(), size_t{4}); QCOMPARE(retained->rows.front().sequence, uint64_t{4});
        QVERIFY(std::is_sorted(retained->rows.begin(), retained->rows.end(), [](const auto& a, const auto& b) { return a.sequence < b.sequence; }));
        // Another section replaces the rows: another source, the clock flag,
        // an unavailable section, or none at all.
        auto other{head_only}; other.source = QStringLiteral("https://other.invalid:18443");
        QVERIFY(B3FlowMeshMarketData::MergeSameHeadObserved(retained, other));
        QCOMPARE(*retained, other);
        retained = head_reply;
        auto suspect{head_only}; suspect.clock_suspect = true;
        QVERIFY(B3FlowMeshMarketData::MergeSameHeadObserved(retained, suspect));
        QCOMPARE(*retained, suspect);
        ObservedTimes unavailable; unavailable.reason = QStringLiteral("observed_time_not_yet_received");
        QVERIFY(B3FlowMeshMarketData::MergeSameHeadObserved(retained, unavailable));
        QCOMPARE(*retained, unavailable);
        QVERIFY(!B3FlowMeshMarketData::MergeSameHeadObserved(retained, unavailable));
        QVERIFY(B3FlowMeshMarketData::MergeSameHeadObserved(retained, std::nullopt));
        QVERIFY(!retained);
        QVERIFY(B3FlowMeshMarketData::MergeSameHeadObserved(retained, head_reply));
        QCOMPARE(*retained, head_reply);
    }
};
QTEST_GUILESS_MAIN(B3FlowMeshTradingTests)
#include "b3flowmeshtradingtests.moc"
