// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/b3flowmeshtradingpanel.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <util/moneystr.h>
#include <QApplication>
#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <limits>
#include <stdexcept>

using B3FlowMeshTrading::Action;
using B3FlowMeshTrading::Market;
using B3FlowMeshTrading::Operation;
namespace {
QLabel* Label(const QString& text, QWidget* parent) {
    auto* label{new QLabel{text, parent}}; label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true); label->setTextInteractionFlags(Qt::TextSelectableByMouse); return label;
}
bool PreparedOperation(Operation op) { return op == Operation::Deposit || op == Operation::Checkpoint || op == Operation::Vault; }
bool SignedAction(Operation op) { return op == Operation::Order || op == Operation::Cancel || op == Operation::Withdraw; }
bool ReadOnly(const std::string& method) {
    return method == "listflowmeshmarkets" || method == "getflowmeshbalance" || method == "getblockchaininfo" ||
        method == "listflowmeshvaultoperations" || method == "testmempoolaccept";
}
QString RpcError(const UniValue& error) {
    const auto& message{error.find_value("message")};
    return error.isObject() && message.isStr() ? QString::fromStdString(message.get_str()).left(500) : QStringLiteral("FlowMesh RPC failed.");
}
} // namespace

struct B3FlowMeshTradingPanel::Result {
    std::optional<Action> action;
    std::optional<B3AssetTransfer::Prepared> prepared;
    std::vector<Market> markets;
    std::vector<UniValue> effects;
    UniValue response;
    QString wallet, error;
    bool broadcast{false}, write_attempted{false};
};

B3FlowMeshTradingPanel::B3FlowMeshTradingPanel(QWidget* parent) : QWidget{parent}
{
    setObjectName(QStringLiteral("flowMeshTradingPanel"));
    auto* outer{new QVBoxLayout{this}}; outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll{new QScrollArea{this}}; scroll->setWidgetResizable(true);
    auto* content{new QWidget{scroll}}; auto* layout{new QVBoxLayout{content}};
    layout->addWidget(Label(tr("FlowMesh — real wallet actions"), content));
    layout->addWidget(Label(tr("Only real node-reported markets and certified balances are shown. There is no fabricated order book or fill history. Base quantities are RAW INTEGER UNITS; 1 B3 = 1,000,000,000 B3 atoms. Accepted requests are not completed trades or payouts."), content));
    auto* form{new QFormLayout}; form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_market = new QComboBox{content}; m_market->setObjectName(QStringLiteral("flowMeshMarket"));
    m_market->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); m_market->setMinimumContentsLength(20);
    form->addRow(tr("Market"), m_market);
    m_refresh = new QPushButton{tr("Refresh market and certified balance"), content}; form->addRow(m_refresh);
    m_status = Label(tr("No wallet selected."), content); form->addRow(m_status);
    m_balances = Label(QString{}, content); form->addRow(m_balances);
    m_side = new QComboBox{content}; m_side->addItems({QStringLiteral("bid"), QStringLiteral("ask")}); form->addRow(tr("Limit side"), m_side);
    const auto entry = [&](const char* name, int length) {
        auto* field{new QLineEdit{content}}; field->setObjectName(QLatin1String(name)); field->setMaxLength(length); return field;
    };
    m_price = entry("flowMeshPriceAtoms", 24); form->addRow(tr("Price: B3 atoms / RAW base unit"), m_price);
    m_quantity = entry("flowMeshRawQuantity", 24); form->addRow(tr("Quantity: RAW base units"), m_quantity);
    layout->addLayout(form);
    const auto pair = [&](QPushButton*& first, const QString& a, QPushButton*& second, const QString& b) {
        auto* row{new QGridLayout}; first = new QPushButton{a, content}; second = new QPushButton{b, content};
        first->setAutoDefault(false); second->setAutoDefault(false); row->addWidget(first, 0, 0); row->addWidget(second, 0, 1); layout->addLayout(row);
    };
    pair(m_order, tr("Review limit order…"), m_cancel_order, tr("Cancel standing side…"));
    auto* funds{new QFormLayout}; funds->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_asset = new QComboBox{content}; m_asset->addItems({tr("Base asset — raw integer units"), tr("Native B3 — decimal B3 units")}); funds->addRow(tr("Deposit / withdrawal asset"), m_asset);
    m_amount = entry("flowMeshFundingAmount", 64); funds->addRow(tr("Amount (units selected above)"), m_amount);
    m_destination = entry("flowMeshWithdrawalAddress", 256); funds->addRow(tr("Withdrawal / payout B3 address"), m_destination);
    layout->addLayout(funds);
    pair(m_deposit, tr("Prepare vault deposit…"), m_withdraw, tr("Request withdrawal…"));
    layout->addWidget(Label(tr("Deposits enter keyless custody and can remain locked without a working validator quorum. A deposit needs 31 confirmations before admission. Withdrawal requests require a later certified checkpoint and a separate on-chain payout."), content));
    auto* admission{new QFormLayout}; admission->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_deposit_txid = entry("flowMeshDepositTxid", 64); admission->addRow(tr("Existing deposit transaction ID"), m_deposit_txid);
    m_deposit_vout = entry("flowMeshDepositVout", 10); m_deposit_vout->setText(QStringLiteral("0")); admission->addRow(tr("Deposit output index"), m_deposit_vout);
    layout->addLayout(admission);
    pair(m_admit, tr("Admit confirmed deposit…"), m_checkpoint, tr("Prepare pending checkpoint…"));
    m_effect = new QComboBox{content}; m_effect->setMinimumContentsLength(20); m_effect->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    layout->addWidget(Label(tr("Connected, unconsumed effects for this wallet (not unconfirmed requests)"), content)); layout->addWidget(m_effect);
    m_publish = new QPushButton{tr("Prepare selected sweep / payout…"), content}; m_publish->setAutoDefault(false); layout->addWidget(m_publish);
    m_review_uncertain = new QPushButton{tr("Review uncertain submission…"), content}; m_review_uncertain->setAutoDefault(false); layout->addWidget(m_review_uncertain);
    m_log = new QPlainTextEdit{content}; m_log->setObjectName(QStringLiteral("flowMeshOperationLog")); m_log->setReadOnly(true); m_log->setMaximumBlockCount(100); m_log->setMinimumHeight(135); layout->addWidget(m_log);
    layout->addStretch(); scroll->setWidget(content); outer->addWidget(scroll);
    connect(m_refresh, &QPushButton::clicked, this, &B3FlowMeshTradingPanel::refresh);
    connect(m_market, &QComboBox::currentIndexChanged, this, [this] { updateMarketText(); if (!m_busy) refresh(); });
    connect(m_order, &QPushButton::clicked, this, [this] { begin(Operation::Order); });
    connect(m_cancel_order, &QPushButton::clicked, this, [this] { begin(Operation::Cancel); });
    connect(m_deposit, &QPushButton::clicked, this, [this] { begin(Operation::Deposit); });
    connect(m_admit, &QPushButton::clicked, this, [this] { begin(Operation::Admit); });
    connect(m_withdraw, &QPushButton::clicked, this, [this] { begin(Operation::Withdraw); });
    connect(m_checkpoint, &QPushButton::clicked, this, [this] { begin(Operation::Checkpoint); });
    connect(m_publish, &QPushButton::clicked, this, [this] { begin(Operation::Vault); });
    connect(m_review_uncertain, &QPushButton::clicked, this, &B3FlowMeshTradingPanel::reviewUncertain);
    connect(m_effect, &QComboBox::currentIndexChanged, this, &B3FlowMeshTradingPanel::updateControls);
    m_timer = new QTimer{this}; m_timer->setInterval(15000);
    connect(m_timer, &QTimer::timeout, this, [this] { if (isVisible()) refresh(); });
    connect(qApp, &QCoreApplication::aboutToQuit, this, &B3FlowMeshTradingPanel::cancelAndWait);
    updateControls();
}

B3FlowMeshTradingPanel::~B3FlowMeshTradingPanel() { cancelAndWait(); }

void B3FlowMeshTradingPanel::setWalletModel(WalletModel* wallet)
{
    cancelAndWait();
    if (m_wallet) disconnect(m_wallet, nullptr, this, nullptr);
    m_wallet = wallet; m_backend.reset(); m_market_data.clear(); m_effect_data.clear();
    m_uncertain_refreshed = false;
    { QSignalBlocker blocker{m_market}; m_market->clear(); } m_effect->clear();
    m_wallet_name = wallet ? wallet->getDisplayName() : tr("No wallet");
    if (wallet) {
        for (auto& candidate : wallet->node().walletLoader().getWallets()) {
            if (candidate->wallet() == wallet->wallet().wallet()) { m_backend = std::move(candidate); break; }
        }
        connect(wallet, &WalletModel::unload, this, [this] { setWalletModel(nullptr); });
        connect(wallet, &QObject::destroyed, this, [this] { setWalletModel(nullptr); });
        m_timer->start();
    }
    m_cancel->store(false); updateMarketText(); refresh();
}

void B3FlowMeshTradingPanel::selectBaseAsset(const QString& asset_id, bool withdrawal)
{
    if (m_busy) { notice(tr("Finish or cancel the current operation before selecting another asset.")); return; }
    m_requested_base = asset_id.toLower(); m_requested_withdrawal = withdrawal; m_route_pending = true;
    m_asset->setCurrentIndex(asset_id.isEmpty() ? 1 : 0);
    refresh();
}

std::optional<Market> B3FlowMeshTradingPanel::market() const
{
    const int i{m_market->currentIndex()};
    if (i < 0 || static_cast<size_t>(i) >= m_market_data.size()) return std::nullopt;
    return m_market_data[i];
}

void B3FlowMeshTradingPanel::notice(const QString& text) { m_log->appendPlainText(text); }

void B3FlowMeshTradingPanel::updateMarketText()
{
    const auto selected{market()};
    if (!selected) { m_status->setText(m_wallet ? tr("No selected market. Refresh reads only real markets; no bootstrap is performed.") : tr("No wallet selected.")); m_balances->clear(); }
    else {
        m_status->setText(tr("Wallet: %1\nMarket: %2\nBase asset ID: %3\nVault: %4\n%5").arg(m_wallet_name, selected->id, selected->base, selected->vault,
            selected->ready ? tr("Runtime ready — not a promise of fills or withdrawal completion.") : selected->reason));
        m_balances->setText(selected->has_account ? tr("Account: %1\nCertified available: %2 raw base units; %3 B3\nReserved: %4 raw base units; %5 B3\nNext certified sequence: %6").arg(selected->account,
            QString::number(selected->base_available), QString::fromStdString(FormatMoney(selected->b3_available)), QString::number(selected->base_reserved),
            QString::fromStdString(FormatMoney(selected->b3_reserved)), QString::number(selected->sequence)) : tr("No trading account yet. Preparing a deposit creates one in this wallet; back it up afterward."));
    }
    if (!m_security_warning.isEmpty()) m_status->setText(m_security_warning + QStringLiteral("\n") + m_status->text());
    updateControls();
}

void B3FlowMeshTradingPanel::updateControls()
{
    const auto selected{market()};
    const bool idle{m_wallet && m_backend && !m_busy && !m_thread && m_security_warning.isEmpty()};
    const bool signing{idle && !m_uncertain && !m_backend->privateKeysDisabled()};
    const bool ready{signing && selected && selected->ready};
    const bool pending{selected && m_pending_sequence && selected->id == m_pending_market && selected->account == m_pending_account && selected->sequence <= *m_pending_sequence};
    m_refresh->setEnabled(idle); m_market->setEnabled(idle);
    for (auto* field : {m_price, m_quantity, m_amount, m_destination, m_deposit_txid, m_deposit_vout}) field->setEnabled(idle);
    m_side->setEnabled(idle); m_asset->setEnabled(idle); m_effect->setEnabled(idle);
    m_order->setEnabled(ready && selected->has_account && !pending); m_cancel_order->setEnabled(ready && selected->has_account && !pending);
    m_withdraw->setEnabled(ready && selected->has_account && !pending); m_deposit->setEnabled(ready);
    m_admit->setEnabled(ready && selected->has_account);
    m_checkpoint->setEnabled(signing && selected && selected->publish_ready && selected->checkpoint_pending);
    m_publish->setEnabled(signing && selected && selected->publish_ready && m_effect->currentIndex() >= 0);
    m_review_uncertain->setVisible(m_uncertain);
    m_review_uncertain->setEnabled(idle && m_uncertain && m_uncertain_refreshed && m_uncertain_backend && m_backend->wallet() == m_uncertain_backend->wallet());
}

void B3FlowMeshTradingPanel::refresh()
{
    if (!m_wallet || !m_backend || m_busy || m_thread) return;
    startJob();
}

bool B3FlowMeshTradingPanel::confirm(const QString& text, bool final_transaction)
{
    m_confirmation = new QMessageBox{QMessageBox::Warning, tr("Review FlowMesh action"), text, QMessageBox::Yes | QMessageBox::Cancel, this};
    const QPointer<QMessageBox> dialog{m_confirmation}; dialog->setTextFormat(Qt::PlainText); dialog->setDefaultButton(QMessageBox::Cancel);
    if (final_transaction) {
        auto* checked{new QCheckBox{tr("I checked the wallet, exact outputs, transaction ID and B3 fee."), dialog}};
        dialog->setCheckBox(checked); dialog->button(QMessageBox::Yes)->setEnabled(false);
        connect(checked, &QCheckBox::toggled, dialog->button(QMessageBox::Yes), &QAbstractButton::setEnabled);
    }
    const bool accepted{dialog->exec() == QMessageBox::Yes};
    if (dialog) delete dialog.data();
    return accepted;
}

void B3FlowMeshTradingPanel::begin(Operation operation)
{
    if (!m_wallet || !m_backend || m_busy || m_thread || m_uncertain || !m_security_warning.isEmpty()) return;
    const auto selected{market()}; if (!selected) return;
    Action a; a.operation = operation; a.market = *selected; a.side = m_side->currentText(); a.native = m_asset->currentIndex() == 1;
    a.destination = m_destination->text();
    try {
        QString error;
        if (operation == Operation::Order) {
            const auto price{B3AssetTransfer::ParseAmount(m_price->text(), 0, &error)}, quantity{B3AssetTransfer::ParseAmount(m_quantity->text(), 0, &error)};
            if (!price || !quantity) throw std::runtime_error{error.toStdString()}; a.price = *price; a.amount = *quantity;
        } else if (operation == Operation::Deposit || operation == Operation::Withdraw) {
            const auto amount{B3AssetTransfer::ParseAmount(m_amount->text(), a.native ? 9 : 0, &error)};
            if (!amount) throw std::runtime_error{error.toStdString()}; a.amount = *amount;
        } else if (operation == Operation::Admit) {
            a.txid = m_deposit_txid->text().toLower(); const QString text{m_deposit_vout->text()};
            bool ok{false}; const auto n{text.toULongLong(&ok)};
            if (!ok || text.isEmpty() || !std::all_of(text.begin(), text.end(), [](QChar c) { return c >= QLatin1Char('0') && c <= QLatin1Char('9'); }) || n > UINT32_MAX) throw std::runtime_error{"Enter an output index from 0 to 4294967295."};
            a.vout = static_cast<uint32_t>(n);
        } else if (operation == Operation::Vault) {
            const int index{m_effect->currentData().toInt()};
            if (m_effect->currentIndex() < 0 || index < 0 || static_cast<size_t>(index) >= m_effect_data.size()) throw std::runtime_error{"Select a connected wallet effect."};
            a.effect = m_effect_data[index];
        }
        B3FlowMeshTrading::Parameters(a);
    } catch (const std::exception& e) { notice(QString::fromUtf8(e.what())); return; }
    catch (const UniValue& e) { notice(RpcError(e)); return; }
    const QPointer<B3FlowMeshTradingPanel> self{this}; const QPointer<WalletModel> wallet{m_wallet}; const auto generation{m_generation};
    m_busy = true; updateControls();
    const bool accepted{confirm(tr("Selected wallet: %1\n\n").arg(m_wallet_name) + B3FlowMeshTrading::Describe(a) +
        (PreparedOperation(operation) ? tr("\n\nThis first step prepares only. You will review the actual B3 fee and exact transaction before broadcasting.") : tr("\n\nSubmit this request ONCE? There is no automatic retry.")), false)};
    if (!self || !wallet || generation != m_generation) return;
    if (!accepted) { m_busy = false; updateControls(); return; }
    if (operation != Operation::Admit) {
        const bool was_locked{m_backend->isLocked()}; m_restore_locked = was_locked; m_relock_backend = was_locked ? m_backend : nullptr; m_relock_wallet_name = m_wallet_name;
        std::unique_ptr<WalletModel::UnlockContext> unlock;
        try { unlock.reset(new WalletModel::UnlockContext{wallet->requestUnlock(WalletModel::UnlockPurpose::General)}); }
        catch (...) {
            if (wallet && was_locked) wallet->setWalletLocked(true);
            if (self && generation == m_generation) { restoreLock(); m_busy = false; notice(tr("Unlock failed. No FlowMesh request was submitted.")); updateControls(); }
            return;
        }
        if (!self || !wallet || generation != m_generation) return;
        if (!unlock->isValid() || m_backend->isLocked()) { unlock.reset(); restoreLock(); m_busy = false; notice(tr("Wallet remains locked. Nothing was submitted.")); updateControls(); return; }
        m_unlock = std::move(unlock);
    }
    startJob(a);
}

void B3FlowMeshTradingPanel::startJob(std::optional<Action> action, std::optional<B3AssetTransfer::Prepared> prepared)
{
    if (!m_wallet || !m_backend || m_thread) return;
    m_busy = true; m_cancel->store(false); updateControls();
    auto result{std::make_shared<Result>()}; result->action = action; result->prepared = prepared; result->broadcast = prepared.has_value(); result->wallet = m_wallet_name;
    m_active_result = result;
    auto* node{&m_wallet->node()}; const auto backend{m_backend}; const auto cancel{m_cancel};
    const auto uri{B3AssetTransfer::WalletUri(m_wallet->getWalletName())}; const auto generation{m_generation};
    const auto selected{market()}; const QString selected_id{selected ? selected->id : QString{}};
    m_thread = QThread::create([node, backend, cancel, uri, result, selected_id] {
        try {
            const auto cancelled = [&] { return cancel->load() || node->shutdownRequested(); };
            const B3FlowMeshTrading::RpcCall rpc = [&](const std::string& method, const UniValue& params) {
                if (cancelled()) throw std::runtime_error{"Operation cancelled."};
                if (!ReadOnly(method)) {
                    if (method != "submitflowmeshdeposit" && (backend->isLocked() || backend->privateKeysDisabled())) throw std::runtime_error{"The captured wallet is locked for spending."};
                    if (method == "sendrawtransaction" || method == "submitflowmeshorder" || method == "cancelflowmeshorder" || method == "requestflowmeshwithdrawal" || method == "submitflowmeshdeposit") result->write_attempted = true;
                }
                return node->executeRpc(method, params, method == "getblockchaininfo" || method == "testmempoolaccept" ? "" : uri);
            };
            if (!result->action) {
                result->markets = B3FlowMeshTrading::ParseMarkets(rpc("listflowmeshmarkets", UniValue{UniValue::VARR}));
                if (!selected_id.isEmpty()) {
                    const auto selected{std::find_if(result->markets.begin(), result->markets.end(), [&](const auto& m) { return m.id == selected_id; })};
                    if (selected != result->markets.end() && selected->has_account) {
                        *selected = B3FlowMeshTrading::ReadMarket(selected_id, rpc);
                        UniValue filter{UniValue::VARR}; filter.push_back(selected_id.toStdString());
                        const auto effects{rpc("listflowmeshvaultoperations", filter)};
                        if (!effects.isArray() || effects.size() > 1000) throw std::runtime_error{"Invalid or oversized connected-effect list."};
                        for (const auto& e : effects.getValues()) {
                            if (e.isObject() && e.find_value("market_id").isStr() && e.find_value("market_id").get_str() == selected_id.toStdString() && e.find_value("account_id").isStr() && e.find_value("account_id").get_str() == selected->account.toStdString()) result->effects.push_back(e);
                        }
                    }
                }
                return;
            }
            auto& a{*result->action};
            B3FlowMeshTrading::CheckSynced(rpc("getblockchaininfo", UniValue{UniValue::VARR}));
            if (result->broadcast) {
                const auto fresh{B3FlowMeshTrading::ReadMarket(a.market.id, rpc)};
                B3FlowMeshTrading::CheckSameMarket(a.market, fresh, false, a.operation == Operation::Checkpoint || a.operation == Operation::Vault);
                B3AssetTransfer::CheckAcceptance(rpc("testmempoolaccept", B3AssetTransfer::AcceptanceParameters(*result->prepared)), *result->prepared);
                result->response = rpc("sendrawtransaction", B3AssetTransfer::BroadcastParameters(*result->prepared));
                if (!result->response.isStr() || result->response.get_str() != result->prepared->txid.toStdString()) throw std::runtime_error{"The node did not confirm the reviewed transaction ID."};
            } else {
                const auto response{B3FlowMeshTrading::DispatchApproved(a, true, cancelled, rpc)};
                if (PreparedOperation(a.operation)) {
                    // A first deposit may create the account. Verify it through
                    // the captured wallet endpoint, never through response labels alone.
                    const auto fresh{B3FlowMeshTrading::ReadMarket(a.market.id, rpc)};
                    B3FlowMeshTrading::CheckSameMarket(a.market, fresh, false, a.operation == Operation::Checkpoint || a.operation == Operation::Vault);
                    if (a.operation == Operation::Deposit) a.market = fresh;
                    result->prepared = B3FlowMeshTrading::ParsePrepared(response, a, [backend](const auto& destination) { return backend->isSpendable(destination); });
                    B3AssetTransfer::CheckAcceptance(rpc("testmempoolaccept", B3AssetTransfer::AcceptanceParameters(*result->prepared)), *result->prepared);
                } else { B3FlowMeshTrading::CheckActionResult(response, a); result->response = response; }
            }
        } catch (const UniValue& error) { result->error = RpcError(error); }
        catch (const std::exception& error) { result->error = QString::fromUtf8(error.what()).left(500); }
        catch (...) { result->error = QStringLiteral("The FlowMesh operation failed."); }
    });
    m_thread->setParent(this); connect(m_thread, &QThread::finished, this, [this, result, generation] { if (generation == m_generation) finishJob(result); }); m_thread->start();
}

void B3FlowMeshTradingPanel::finishJob(const std::shared_ptr<Result>& result)
{
    stopWorker(); m_active_result.reset();
    if (!m_wallet || m_cancel->load()) { restoreLock(); m_busy = false; updateControls(); return; }
    if (!result->error.isEmpty()) {
        restoreLock(); m_busy = false;
        if (result->write_attempted) markUncertain(result);
        notice(tr("Wallet: %1\n%2\n%3").arg(result->wallet, result->error, result->write_attempted
            ? tr("Submission outcome may be unknown. Refresh this captured wallet, inspect its state, then use Review uncertain submission to acknowledge it. No retry occurred.")
            : tr("No order, deposit admission, withdrawal request or transaction was submitted. Preparation may have created a wallet account/address; back up the wallet.")));
        if (result->prepared) notice(tr("Reviewed transaction ID: %1").arg(result->prepared->txid));
        if (!result->action) { m_uncertain_refreshed = false; m_market_data.clear(); QSignalBlocker block{m_market}; m_market->clear(); m_effect->clear(); }
        updateMarketText(); return;
    }
    if (!result->action) {
        if (m_uncertain && m_uncertain_backend && m_backend->wallet() == m_uncertain_backend->wallet()) m_uncertain_refreshed = true;
        const QString previous{market() ? market()->id : QString{}}; m_market_data = result->markets; m_effect_data = result->effects;
        { QSignalBlocker block{m_market}; m_market->clear(); int selected{-1};
            for (size_t i{0}; i < m_market_data.size(); ++i) {
                const auto& m{m_market_data[i]}; m_market->addItem(m.base.left(16) + QStringLiteral("… / B3"), m.id);
                if (m_route_pending && !m_requested_base.isEmpty() ? m.base == m_requested_base : m.id == previous) selected = static_cast<int>(i);
            }
            if (m_route_pending && !m_requested_base.isEmpty() && selected == -1) { m_market->setCurrentIndex(-1); notice(tr("No established market was reported for the selected asset. This panel does not bootstrap one.")); }
            else if (selected >= 0) m_market->setCurrentIndex(selected);
        }
        { QSignalBlocker block{m_effect}; m_effect->clear(); const auto selected{market()};
            for (size_t i{0}; selected && i < m_effect_data.size(); ++i) {
                const auto& e{m_effect_data[i]}; if (e.find_value("market_id").get_str() != selected->id.toStdString()) continue;
                const auto& kind{e.find_value("kind")}; const auto& id{e.find_value("effect_id")};
                if (kind.isStr() && id.isStr()) m_effect->addItem(QString::fromStdString(kind.get_str()) + QStringLiteral(" — ") + QString::fromStdString(id.get_str()).left(20), static_cast<int>(i));
            }
        }
        if (m_route_pending) { m_asset->setCurrentIndex(m_requested_base.isEmpty() ? 1 : 0); (m_requested_withdrawal ? m_destination : m_amount)->setFocus(); m_route_pending = false; }
        const auto selected_now{market()};
        const bool changed_selection{selected_now && selected_now->id != previous};
        m_busy = false; updateMarketText();
        if (changed_selection) refresh(); // Fill the newly selected account/effects immediately.
        return;
    }
    const auto& a{*result->action};
    if (result->prepared && !result->broadcast) {
        const QPointer<B3FlowMeshTradingPanel> self{this}; const auto generation{m_generation};
        const bool accepted{confirm(tr("Wallet: %1\n\n%2\n\nActual B3 network fee: %3 B3\nExact transaction ID: %4\n\nSubmit these exact signed bytes ONCE?")
            .arg(result->wallet, B3FlowMeshTrading::Describe(a), QString::fromStdString(FormatMoney(result->prepared->fee)), result->prepared->txid), true)};
        if (!self || generation != m_generation) return;
        if (!accepted) { restoreLock(); m_busy = false; notice(tr("Prepared transaction cancelled; nothing was broadcast. Back up any account/address created during preparation.")); updateControls(); return; }
        startJob(a, result->prepared); return;
    }
    restoreLock(); m_busy = false;
    if (result->broadcast) {
        notice(tr("Wallet: %1\nTransaction submitted: %2\nWait for on-chain confirmation; this is not a guarantee of market settlement or payout finality.").arg(result->wallet, result->prepared->txid));
        if (a.operation == Operation::Deposit) { m_deposit_txid->setText(result->prepared->txid); m_deposit_vout->setText(QStringLiteral("0")); }
    } else {
        if (SignedAction(a.operation)) { m_pending_market = a.market.id; m_pending_account = a.market.account; m_pending_sequence = a.market.sequence; }
        notice(tr("Wallet: %1\nRequest accepted, not yet proof of execution. Action ID: %2\nRefresh certified balances and connected effects; no automatic retry.")
            .arg(result->wallet, QString::fromStdString(result->response.find_value("action_id").get_str())));
    }
    updateMarketText(); refresh();
}

void B3FlowMeshTradingPanel::stopWorker()
{
    if (!m_thread) return; disconnect(m_thread, nullptr, this, nullptr); m_thread->wait(); delete m_thread; m_thread = nullptr;
}
void B3FlowMeshTradingPanel::cancelAndWait()
{
    m_timer->stop(); m_cancel->store(true); ++m_generation;
    if (m_confirmation) m_confirmation->reject();
    stopWorker();
    // Draining a running write does not prove it was cancelled. Preserve its
    // public identity even if a wallet switch disconnects the completion slot.
    if (m_active_result && m_active_result->write_attempted) {
        markUncertain(m_active_result);
        notice(tr("Wallet changed or operation closed after submission began. Inspect this saved request; it was not retried.\n%1").arg(m_uncertain_details));
    }
    m_active_result.reset(); restoreLock(); m_busy = false;
}
void B3FlowMeshTradingPanel::markUncertain(const std::shared_ptr<Result>& result)
{
    m_uncertain = true; m_uncertain_refreshed = false; m_uncertain_backend = m_backend;
    m_uncertain_details = tr("Captured wallet: %1").arg(result->wallet);
    if (result->action) m_uncertain_details += QStringLiteral("\n") + B3FlowMeshTrading::Describe(*result->action);
    if (result->prepared) m_uncertain_details += tr("\nExact transaction ID: %1").arg(result->prepared->txid);
}
void B3FlowMeshTradingPanel::reviewUncertain()
{
    if (!m_wallet || !m_backend || m_busy || m_thread || !m_uncertain || !m_uncertain_refreshed ||
        !m_security_warning.isEmpty() || !m_uncertain_backend || m_backend->wallet() != m_uncertain_backend->wallet()) return;
    const QPointer<B3FlowMeshTradingPanel> self{this}; const auto generation{m_generation};
    m_busy = true; updateControls();
    const bool accepted{confirm(m_uncertain_details + tr("\n\nA fresh read-only refresh succeeded, but this does NOT prove whether the submission was accepted. Check the saved transaction ID or certified account sequence in wallet/node state.\n\nI have inspected the outcome and want to re-enable NEW actions. This acknowledgement never resends the saved request."), false)};
    if (!self || generation != m_generation) return;
    if (accepted) { m_uncertain = false; m_uncertain_refreshed = false; m_uncertain_backend.reset(); notice(tr("Uncertain submission acknowledged after inspection. Nothing was replayed.")); }
    m_busy = false; updateControls();
}
bool B3FlowMeshTradingPanel::restoreLock()
{
    m_unlock.reset(); if (!m_restore_locked) return true;
    try {
        if (m_relock_backend && !m_relock_backend->isLocked()) m_relock_backend->lock();
        if (m_relock_backend && m_relock_backend->isLocked()) { m_restore_locked = false; m_relock_backend.reset(); return true; }
    } catch (...) { }
    m_security_warning = tr("SECURITY: spending relock could not be verified for wallet %1. Lock it or close the app before leaving it unattended. FlowMesh controls are blocked.").arg(m_relock_wallet_name);
    notice(m_security_warning); m_status->setText(m_security_warning); updateControls(); Q_EMIT securityWarning(m_security_warning); return false;
}
