// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <qt/b3assetsenddialog.h>

#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <qt/b3fixed.h>
#include <univalue.h>
#include <util/moneystr.h>

#include <QCheckBox>
#include <QCoreApplication>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QThread>
#include <QVBoxLayout>

#include <stdexcept>
#include <utility>

namespace {

QLabel* PlainLabel(const QString& text, QWidget* parent)
{
    auto* label{new QLabel{text, parent}};
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

QString SafeError(const UniValue& error)
{
    // In-process wallet errors are useful (insufficient B3, unsupported owner,
    // inactive assets). Never show a serialized response or signed transaction.
    if (error.isObject()) {
        const auto& message{error.find_value("message")};
        if (message.isStr()) return QString::fromStdString(message.get_str()).left(500);
    }
    return QStringLiteral("The wallet could not complete the asset operation.");
}

void CheckReady(interfaces::Node& node, interfaces::Wallet& wallet,
                const std::shared_ptr<std::atomic_bool>& cancel)
{
    if (cancel->load() || node.shutdownRequested()) throw std::runtime_error{"The asset operation was cancelled."};
    if (wallet.privateKeysDisabled() || wallet.isLocked()) {
        throw std::runtime_error{"The captured wallet is locked for spending. Unlock it normally before preparing another asset transfer."};
    }
}

void CheckSynced(interfaces::Node& node)
{
    const UniValue info{node.executeRpc("getblockchaininfo", UniValue{UniValue::VARR}, "")};
    if (!info.isObject()) throw std::runtime_error{"The node did not return valid chain state."};
    const auto& ibd{info.find_value("initialblockdownload")};
    const auto& blocks{info.find_value("blocks")};
    const auto& headers{info.find_value("headers")};
    if (!ibd.isBool() || ibd.get_bool() || !blocks.isNum() || !headers.isNum() ||
        blocks.getInt<int64_t>() < 0 || headers.getInt<int64_t>() > blocks.getInt<int64_t>()) {
        throw std::runtime_error{"Wait for the node to finish synchronizing before sending assets."};
    }
}

} // namespace

struct B3AssetSendDialog::JobResult {
    std::optional<B3AssetTransfer::Prepared> prepared;
    QString error;
    bool broadcast_attempted{false};
    bool broadcast_succeeded{false};
};

B3AssetSendDialog::B3AssetSendDialog(WalletModel* wallet, const B3AssetRecord& asset, QWidget* parent)
    : QDialog{parent}, m_wallet{wallet}, m_asset{asset},
      m_wallet_name{wallet ? wallet->getWalletName() : QString{}},
      m_wallet_display{wallet ? wallet->getDisplayName() : QString{}},
      m_wallet_uri{B3AssetTransfer::WalletUri(m_wallet_name)},
      m_node{wallet ? &wallet->node() : nullptr}
{
    setObjectName(QStringLiteral("b3AssetSendDialog"));
    setWindowTitle(tr("Send asset"));
    setModal(true);
    resize(650, 570);
    auto* layout{new QVBoxLayout{this}};
    layout->addWidget(PlainLabel(tr("Prepare the transfer, check the exact asset and network fee, then confirm sending."), this));
    auto* form{new QFormLayout};
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->addRow(tr("Wallet"), PlainLabel(m_wallet_display + (m_wallet_name.isEmpty() ? tr(" (unnamed wallet)") : QStringLiteral("\n") + m_wallet_name), this));
    form->addRow(tr("Asset"), PlainLabel(m_asset.display_name.isEmpty() ? tr("Unnamed asset") : m_asset.display_name, this));
    form->addRow(tr("Ticker"), PlainLabel(m_asset.ticker.isEmpty() ? tr("Not supplied") : m_asset.ticker, this));
    auto* asset_id{new QLineEdit{m_asset.asset_id, this}};
    asset_id->setObjectName(QStringLiteral("assetId"));
    asset_id->setReadOnly(true);
    form->addRow(tr("Full asset ID"), asset_id);
    m_address = new QLineEdit{this};
    m_address->setObjectName(QStringLiteral("assetRecipient"));
    m_address->setMaxLength(256);
    m_address->setPlaceholderText(tr("Recipient B3 address"));
    form->addRow(tr("Recipient"), m_address);
    m_amount = new QLineEdit{this};
    m_amount->setObjectName(QStringLiteral("assetAmount"));
    m_amount->setMaxLength(64);
    form->addRow(m_asset.precision_known ? tr("Amount") : tr("Amount (raw integer units)"), m_amount);
    if (m_asset.precision_known) {
        layout->addWidget(PlainLabel(tr("Verified asset precision: %1 decimal places.").arg(m_asset.decimals), this));
    } else {
        layout->addWidget(PlainLabel(tr("Display precision is unknown. Enter raw integer units only; 1 means one smallest asset unit. A ticker does not establish precision."), this));
    }
    m_review_amount = PlainLabel(tr("Not prepared"), this);
    m_review_amount->setObjectName(QStringLiteral("assetReviewAmount"));
    form->addRow(tr("Exact transfer"), m_review_amount);
    m_fee = PlainLabel(tr("Calculated during preparation; paid in B3"), this);
    m_fee->setObjectName(QStringLiteral("assetNetworkFee"));
    form->addRow(tr("Network fee"), m_fee);
    m_txid = new QLineEdit{this};
    m_txid->setObjectName(QStringLiteral("assetTransactionId"));
    m_txid->setReadOnly(true);
    form->addRow(tr("Transaction ID"), m_txid);
    layout->addLayout(form);
    m_confirm = new QCheckBox{tr("I checked this wallet, the full asset ID, amount, recipient and B3 fee."), this};
    m_confirm->setObjectName(QStringLiteral("assetSendConfirmation"));
    m_confirm->setEnabled(false);
    layout->addWidget(m_confirm);
    m_status = PlainLabel(tr("Only mature, safe asset inputs are selected. Keep enough native B3 available for the fee."), this);
    m_status->setObjectName(QStringLiteral("assetSendStatus"));
    layout->addWidget(m_status);
    layout->addStretch();
    auto* buttons{new QHBoxLayout};
    buttons->addStretch();
    m_action = new QPushButton{tr("Prepare transfer"), this};
    m_action->setObjectName(QStringLiteral("assetSendAction"));
    // Enter/double-click must never turn a preparation into confirmation.
    m_action->setAutoDefault(false);
    m_action->setDefault(false);
    m_close = new QPushButton{tr("Cancel"), this};
    m_close->setAutoDefault(false);
    buttons->addWidget(m_close);
    buttons->addWidget(m_action);
    layout->addLayout(buttons);
    connect(m_close, &QPushButton::clicked, this, &B3AssetSendDialog::reject);
    connect(m_action, &QPushButton::clicked, this, [this] {
        if (m_phase == Phase::Editing) prepare();
        else if (m_phase == Phase::Review) submit();
    });
    connect(m_confirm, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_phase == Phase::Review) m_action->setEnabled(checked);
    });
    connect(qApp, &QCoreApplication::aboutToQuit, this, &B3AssetSendDialog::cancelAndWait);
    if (m_wallet) {
        connect(m_wallet, &WalletModel::unload, this, &B3AssetSendDialog::cancelAndWait);
        connect(m_wallet, &QObject::destroyed, this, &B3AssetSendDialog::cancelAndWait);
        // Capture a separate interface sharing the exact backend wallet. Its
        // lifetime is independent of WalletModel; worker threads never touch
        // a Qt wallet object and cannot silently switch to a same-named wallet.
        for (auto& candidate : m_node->walletLoader().getWallets()) {
            if (candidate->getWalletName() == m_wallet_name.toStdString() &&
                candidate->wallet() == m_wallet->wallet().wallet()) {
                m_backend = std::move(candidate);
                break;
            }
        }
    }
    const QString validation{B3AssetTransfer::ValidateAsset(m_asset)};
    if (!m_wallet || !m_backend || !validation.isEmpty() || m_backend->privateKeysDisabled()) {
        m_action->setEnabled(false);
        setStatus(!validation.isEmpty() ? validation : tr("A loaded wallet with spending keys is required for this asset transfer."));
    }
}

B3AssetSendDialog::~B3AssetSendDialog()
{
    m_cancel->store(true);
    stopWorker();
    m_unlock.reset();
}

int B3AssetSendDialog::execForAsset(WalletModel* wallet, const B3AssetRecord& asset, QWidget* parent)
{
    QPointer<B3AssetSendDialog> dialog{new B3AssetSendDialog{wallet, asset, parent}};
    const int result{dialog->exec()};
    if (dialog) delete dialog.data();
    return result;
}

void B3AssetSendDialog::setStatus(const QString& text)
{
    m_status->setText(text);
}

void B3AssetSendDialog::setEditing(const bool editing)
{
    m_amount->setReadOnly(!editing);
    m_address->setReadOnly(!editing);
}

void B3AssetSendDialog::prepare()
{
    if (m_phase != Phase::Editing || !m_wallet || !m_backend || m_thread) return;
    QString error;
    const auto amount{B3AssetTransfer::ParseAmount(m_amount->text(), m_asset.precision_known ? m_asset.decimals : 0, &error)};
    if (!amount) {
        setStatus(error);
        return;
    }
    const QString recipient{m_address->text()};
    try {
        B3AssetTransfer::PrepareParameters(m_asset, *amount, recipient);
    } catch (const std::exception& e) {
        setStatus(QString::fromUtf8(e.what()));
        return;
    }
    m_phase = Phase::Preparing;
    m_cancel->store(false);
    m_close_requested = false;
    m_action->setEnabled(false);
    setEditing(false);
    const QPointer<B3AssetSendDialog> self{this};
    const QPointer<WalletModel> wallet{m_wallet};
    const bool was_locked{m_backend->isLocked()};
    // Keep the context on the GUI thread. The guard also covers parent/wallet
    // deletion while the synchronous passphrase dialog processes events.
    std::unique_ptr<WalletModel::UnlockContext> unlocked;
    try {
        unlocked.reset(new WalletModel::UnlockContext{wallet->requestUnlock(WalletModel::UnlockPurpose::General)});
    } catch (...) {
        // A failing prompt may have unlocked before throwing, before a context
        // existed. Restore the state captured at this operation's boundary.
        if (wallet && was_locked) wallet->setWalletLocked(true);
        if (self && m_phase == Phase::Preparing && !m_cancel->load()) {
            m_phase = Phase::Editing;
            setEditing(true);
            m_action->setEnabled(true);
            setStatus(tr("The wallet unlock request did not complete. Nothing was submitted."));
        }
        return;
    }
    if (!self || !wallet) return;
    if (m_cancel->load() || m_phase != Phase::Preparing) return;
    if (!unlocked->isValid() || m_backend->isLocked()) {
        m_phase = Phase::Editing;
        setEditing(true);
        m_action->setEnabled(true);
        setStatus(tr("The wallet was not unlocked for spending. Nothing was submitted."));
        return;
    }
    m_unlock = std::move(unlocked);
    m_raw_amount = *amount;
    m_recipient = recipient;
    setStatus(tr("Checking current balances and preparing a signed transfer. Nothing is being broadcast."));
    startJob(false);
}

void B3AssetSendDialog::submit()
{
    if (m_phase != Phase::Review || !m_confirm->isChecked() || !m_prepared ||
        !m_wallet || !m_backend || m_thread || m_cancel->load()) return;
    if (m_backend->isLocked()) {
        m_phase = Phase::Finished;
        m_action->setEnabled(false);
        m_confirm->setEnabled(false);
        m_unlock.reset();
        setStatus(tr("The wallet was locked after preparation. Nothing was submitted. Close this dialog and explicitly prepare again after unlocking."));
        return;
    }
    // Change phase and disable both controls before queuing work, so a second
    // click or queued signal cannot submit another transaction.
    m_phase = Phase::Submitting;
    m_action->setEnabled(false);
    m_confirm->setEnabled(false);
    setStatus(tr("Submitting this exact transaction once. Its ID is shown above; do not start another send while the outcome is pending."));
    startJob(true);
}

void B3AssetSendDialog::startJob(const bool broadcast)
{
    const auto result{std::make_shared<JobResult>()};
    if (broadcast) result->prepared = m_prepared;
    auto* const node{m_node};
    const auto backend{m_backend};
    const auto cancel{m_cancel};
    const auto asset{m_asset};
    const auto uri{m_wallet_uri};
    const auto amount{m_raw_amount};
    const auto recipient{m_recipient};
    m_thread = QThread::create([node, backend, cancel, asset, uri, amount, recipient, result, broadcast] {
        try {
            CheckReady(*node, *backend, cancel);
            CheckSynced(*node);
            CheckReady(*node, *backend, cancel);
            if (!broadcast) {
                UniValue balances_params{UniValue::VARR};
                balances_params.push_back(asset.asset_id.toStdString());
                balances_params.push_back(1);
                balances_params.push_back(false);
                B3AssetTransfer::CheckAvailable(node->executeRpc("getwalletassets", balances_params, uri), asset, amount);
                CheckReady(*node, *backend, cancel);
                result->prepared = B3AssetTransfer::ParsePrepared(
                    node->executeRpc("sendasset", B3AssetTransfer::PrepareParameters(asset, amount, recipient), uri),
                    asset, amount, recipient);
            }
            CheckReady(*node, *backend, cancel);
            B3AssetTransfer::CheckChangeOwnership(*result->prepared,
                [backend](const CTxDestination& destination) { return backend->isSpendable(destination); });
            B3AssetTransfer::CheckAcceptance(
                node->executeRpc("testmempoolaccept", B3AssetTransfer::AcceptanceParameters(*result->prepared), ""),
                *result->prepared);
            if (broadcast) {
                // Last-moment actual backend lock check, with no lock held
                // across RPC. The immutable signed bytes are never regenerated.
                CheckReady(*node, *backend, cancel);
                result->broadcast_attempted = true;
                const UniValue response{node->executeRpc("sendrawtransaction", B3AssetTransfer::BroadcastParameters(*result->prepared), uri)};
                if (!response.isStr() || QString::fromStdString(response.get_str()) != result->prepared->txid) {
                    throw std::runtime_error{"The node did not confirm the expected transaction ID."};
                }
                result->broadcast_succeeded = true;
            }
        } catch (const UniValue& error) {
            result->error = SafeError(error);
        } catch (const std::exception& error) {
            result->error = QString::fromUtf8(error.what()).left(500);
        } catch (...) {
            result->error = QStringLiteral("The wallet could not complete the asset operation.");
        }
    });
    m_thread->setParent(this);
    const uint64_t generation{++m_generation};
    connect(m_thread, &QThread::finished, this, [this, result, generation] {
        if (m_generation == generation) finishJob(result);
    });
    m_thread->start();
}

void B3AssetSendDialog::finishJob(const std::shared_ptr<JobResult>& result)
{
    stopWorker();
    if (!m_wallet || (m_close_requested && !result->broadcast_attempted)) {
        m_unlock.reset();
        QDialog::reject();
        return;
    }
    if (result->broadcast_attempted) {
        m_phase = Phase::Finished;
        m_unlock.reset();
        m_action->setEnabled(false);
        m_close->setText(tr("Close"));
        if (result->broadcast_succeeded) {
            setStatus(tr("Transaction submitted. Track the transaction ID shown above for confirmation."));
            Q_EMIT transactionSubmitted(result->prepared->txid);
        } else {
            setStatus(tr("Submission outcome may be unknown. Transaction ID: %1\n%2\nCheck wallet history and node state before another send. This dialog will not retry or regenerate the transaction.")
                          .arg(result->prepared->txid, result->error));
            Q_EMIT submissionUncertain(result->prepared->txid);
        }
        return;
    }
    if (!result->error.isEmpty()) {
        m_unlock.reset();
        m_prepared.reset();
        if (m_phase == Phase::Submitting) {
            m_phase = Phase::Finished;
            m_action->setEnabled(false);
            m_close->setText(tr("Close"));
            setStatus(tr("Nothing was submitted. %1\nClose this dialog before explicitly preparing another transfer.").arg(result->error));
        } else {
            m_phase = Phase::Editing;
            setEditing(true);
            m_action->setEnabled(true);
            setStatus(tr("Nothing was submitted. %1").arg(result->error));
        }
        return;
    }
    if (m_cancel->load() || m_backend->isLocked()) {
        m_unlock.reset();
        m_phase = Phase::Finished;
        setStatus(tr("The operation was cancelled or the wallet locked. Nothing was submitted."));
        return;
    }
    m_prepared = result->prepared;
    m_phase = Phase::Review;
    m_review_amount->setText(m_asset.precision_known
        ? tr("%1 %2 (%3 raw units)").arg(B3Fixed::format(m_prepared->raw_amount, m_asset.decimals), m_asset.ticker, QString::number(m_prepared->raw_amount))
        : tr("%1 raw integer units (display precision unknown)").arg(m_prepared->raw_amount));
    m_fee->setText(QString::fromStdString(FormatMoney(m_prepared->fee)) + QStringLiteral(" B3"));
    m_txid->setText(m_prepared->txid);
    m_confirm->setChecked(false);
    m_confirm->setEnabled(true);
    m_action->setText(tr("Send this transaction"));
    m_action->setEnabled(false);
    setStatus(tr("Review the full asset ID, recipient, exact amount and B3 fee above. This signed transaction has not been broadcast."));
}

void B3AssetSendDialog::stopWorker()
{
    if (!m_thread) return;
    // Ordinary completion calls this only after finished. Cancellation at
    // wallet detach/application shutdown must drain before Node is destroyed.
    // The worker never waits for the GUI or performs automatic retry loops.
    disconnect(m_thread, nullptr, this, nullptr);
    m_thread->requestInterruption();
    m_thread->wait();
    delete m_thread;
    m_thread = nullptr;
}

void B3AssetSendDialog::reject()
{
    m_cancel->store(true);
    if (m_thread) {
        m_close_requested = true;
        m_action->setEnabled(false);
        m_confirm->setEnabled(false);
        setStatus(m_phase == Phase::Submitting
            ? tr("Waiting for the current submission result. Its outcome cannot be cancelled once submitted; do not retry.")
            : tr("Cancelling after the current wallet operation finishes. Nothing will be submitted."));
        return;
    }
    m_unlock.reset();
    QDialog::reject();
}

void B3AssetSendDialog::cancelAndWait()
{
    m_cancel->store(true);
    m_close_requested = true;
    m_phase = Phase::Finished;
    ++m_generation;
    stopWorker();
    m_unlock.reset();
    QDialog::reject();
}
