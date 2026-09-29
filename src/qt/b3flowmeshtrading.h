// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_QT_B3FLOWMESHTRADING_H
#define BITCOIN_QT_B3FLOWMESHTRADING_H

#include <qt/b3assettransfer.h>
#include <univalue.h>
#include <QString>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace B3FlowMeshTrading {
struct Market {
    QString id, base, vault, domain, config, account, checkpoint, reason;
    bool ready{false}, publish_ready{false}, has_account{false}, checkpoint_pending{false}, remote{false};
    uint64_t sequence{0};
    CAmount base_available{0}, base_reserved{0}, b3_available{0}, b3_reserved{0};
};
enum class Operation { Order, Cancel, Deposit, Admit, Withdraw, Checkpoint, Vault };
struct Action {
    Operation operation{Operation::Order};
    Market market;
    QString side, destination, txid;
    CAmount price{0}, amount{0};
    bool native{false};
    bool inverse_display{false};
    //! Presentation-only evidence of conservative grid conversion. Neither is
    //! serialized or included in Parameters: price is the reviewed commitment.
    bool display_limit_adjusted{false};
    QString entered_display_limit;
    std::optional<int> display_decimals;
    QString display_ticker;
    uint32_t vout{0};
    UniValue effect;
    //! A positive bounded wait (milliseconds) for the new signed action's
    //! certified inclusion, passed as the RPC's trailing wait_ms: an order,
    //! cancel or withdrawal then returns verified in the same call when the
    //! remote endpoint answers in time. Latency only; never reviewed economics.
    int submit_wait_ms{0};
    //! Presentation only: how a marketable-limit ticket priced this order.
    //! Never serialized or included in Parameters; price is the commitment.
    struct Marketable {
        CAmount reference{0}, visible{0};
        int slippage_bps{0};
        QString reference_display;
    };
    std::optional<Marketable> marketable;
    //! Presentation only: the account's current order on this side, which a
    //! SUBMIT atomically replaces (its quantity then counts from zero).
    struct Replaces {
        CAmount remaining{0}, filled{0};
        std::optional<CAmount> limit; // Empty for a curve that is not a simple limit.
        size_t points{0};
    };
    std::optional<Replaces> replaces;
};
struct Request { std::string method; UniValue params{UniValue::VARR}; };
//! A verified per-action outcome as the local RPC reports it (never an
//! endpoint claim: the node strips any outcome it did not verify).
struct Outcome {
    //! applied | rejected | included_not_executed | not_executed
    QString status, reason_code, reason_text, evidence, microblock_hash;
    bool final{false};
    uint64_t microblock_sequence{0};
    struct Fill {
        QString side;
        bool cleared{false}, superseded_in_entry{false};
        CAmount quantity{0}, price{0}, quote_atoms{0}, fee_atoms{0}, net_quote_atoms{0};
    };
    std::optional<Fill> fill;
    std::optional<CAmount> withdrawal_amount, deposit_amount;
    QString consumed_by;
    uint64_t consumed_in_sequence{0};
};
struct Receipt {
    QString action_id, state, reason, endpoint, microblock_hash, market, account;
    //! Display only: a reason name (unknown names are shown as unrecognised
    //! and never treated as terminal) and the backend's stage label.
    QString reason_code, stage;
    bool certificate_verified{false}, outcome_verified{false};
    //! Sticky local history, distinct from fresh certificate verification.
    bool no_resubmit{false};
    uint64_t microblock_sequence{0};
    //! Present exactly when outcome_verified.
    std::optional<Outcome> outcome;
    bool Included() const { return state == QStringLiteral("certified_inclusion") && certificate_verified; }
    //! A verified outcome that can never change.
    bool Terminal() const { return outcome_verified && outcome && outcome->final; }
    //! Proven never to execute: a different certified action used its sequence.
    bool NotExecuted() const { return outcome_verified && outcome && outcome->status == QStringLiteral("not_executed"); }
};
struct SavedAction {
    struct Point { CAmount price{0}, quantity{0}; bool operator==(const Point&) const = default; };
    QString market, domain, config, account, signed_bytes_sha256;
    QString canonical_side;
    std::vector<Point> canonical_points;
    Receipt receipt;
    std::optional<uint64_t> sequence;
    uint64_t initial_submission_ms{0}, signed_bytes_size{0};
    uint8_t type{0};
    bool may_have_been_sent{false};
};
struct SavedActions {
    QString account;
    std::vector<SavedAction> actions;
};
using RpcCall = std::function<UniValue(const std::string&, const UniValue&)>;

//! All parsers fail closed with std::runtime_error / structured RPC exceptions.
Market ParseMarket(const UniValue& value);
std::vector<Market> ParseMarkets(const UniValue& value);
Market ReadMarket(const QString& id, const RpcCall& rpc);
void CheckSameMarket(const Market& reviewed, const Market& fresh, bool sequence, bool settlement = false);
void CheckSynced(const UniValue& chain);
Request Parameters(const Action& action);
//! The wait a submission may carry: `wait_ms` only for a remote order, cancel
//! or withdrawal of a wallet that was already unlocked, so waiting never
//! extends how long the panel keeps a wallet it unlocked for the request open.
int SubmissionWaitMs(Operation operation, bool remote, bool wallet_was_unlocked, int wait_ms);
//! No calls at all without approval; cancellation is checked before the write.
UniValue DispatchApproved(const Action& action, bool approved,
                         const std::function<bool()>& cancelled, const RpcCall& rpc);
QString Describe(const Action& action);
//! Validate signed bytes, independently bind every output and actual fee review.
B3AssetTransfer::Prepared ParsePrepared(const UniValue& value, const Action& action,
    const std::function<bool(const CTxDestination&)>& is_spendable);
void CheckActionResult(const UniValue& value, const Action& action);
Receipt ParseReceipt(const UniValue& value, const QString& market, const QString& expected_action = {});
QString DescribeReceipt(const Receipt& receipt);
//! Strict parse of a verified outcome object (fails closed).
Outcome ParseOutcome(const UniValue& value);
QString DescribeOutcome(const Outcome& outcome);
//! A request row's progress. Signed, Sent and Admitted are historical facts;
//! Certified and OutcomeVerified follow only current verified evidence.
enum class Stage : uint8_t { Signing, Signed, Sent, Admitted, Certified, OutcomeVerified };
enum class StageFlag : uint8_t { None, Uncertain, Refused, CertifiedEarlier, NotExecuted };
//! What the request asked for: only an order can report a fill.
enum class RequestKind : uint8_t { Order, Cancel, Other };
struct StageView {
    Stage reached{Stage::Signing};
    StageFlag flag{StageFlag::None};
    //! A verified final outcome or a proven non-execution: it never changes.
    bool terminal{false};
    //! Nothing further is expected for display: verified inclusion, a verified
    //! outcome, a proven non-execution or retained certification.
    bool settled{false};
    //! One line for the row; never an RPC name.
    QString status;
    //! The verified-outcome cell: empty until certified, "not reported" when
    //! inclusion is verified but no per-action outcome is.
    QString outcome;
};
//! Maps the backend's display stage (node::FlowMeshReceiptStage, the single
//! source) and never shows more than the receipt proves: Certified needs
//! verified inclusion (or the retained no-resubmit record, labelled
//! "earlier"), a verified outcome needs outcome_verified, and "not executed"
//! needs a verified outcome. A reported refusal or an unknown outcome is
//! never terminal. A stage the receipt does not support falls back to its state.
StageView DescribeStage(const Receipt& receipt, bool local, RequestKind kind,
                        std::optional<int> decimals = {}, const QString& ticker = {});
QString StageName(Stage stage);
//! A short request title in exact display units ("Buy 1 tUSD @ 1 B3/tUSD",
//! "Cancel Sell B3"); never an RPC name or a guessed amount.
QString ShortTitle(const Action& action);
QString ShortTitle(const SavedAction& action, std::optional<int> decimals, const QString& ticker, bool inverse);
RequestKind KindOf(Operation operation);
RequestKind KindOf(const SavedAction& action);
SavedActions ParseSavedActions(const UniValue& value);
//! Metadata enrichment is optional and must be bound to the exact market by
//! the caller. The canonical side and raw points remain independently visible.
QString DescribeSavedAction(const SavedAction& action, std::optional<int> decimals = {}, const QString& ticker = {});
//! Exact-object status/retry never includes economics, a nonce or signing inputs.
//! A positive wait (status reads only, never a retry) asks a remote client to
//! hold the read open that long, bounded, for certified inclusion.
Request ReceiptParameters(const QString& market, const QString& action_id, bool retry = false, int wait_ms = 0);
} // namespace B3FlowMeshTrading
#endif
