// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <chainparams.h>
#include <flowmesh/market.h>
#include <flowmesh/auth.h>
#include <flowmesh/microblock.h>
#include <key_io.h>
#include <qt/b3flowmeshtrading.h>
#include <qt/b3flowmeshmarketdata.h>
#include <core_io.h>
#include <rpc/util.h>
#include <streams.h>
#include <test/util/flowmesh.h>
#include <util/int128.h>
#include <util/translation.h>
#include <QTest>
#include <array>
#include <stdexcept>

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
        UniValue legacy{UniValue::VOBJ}; legacy.pushKV("market_id", market.toStdString()); legacy.pushKV("action_id", id.toStdString()); legacy.pushKV("accepted", true);
        const auto receipt{ParseReceipt(legacy, market)}; QCOMPARE(receipt.state, QStringLiteral("queued")); QVERIFY(!receipt.Included());
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
        QVERIFY(approximate.startsWith(QStringLiteral("≈")));
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
};
QTEST_GUILESS_MAIN(B3FlowMeshTradingTests)
#include "b3flowmeshtradingtests.moc"
