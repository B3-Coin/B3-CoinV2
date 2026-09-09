// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <qt/b3flowmeshpanel.h>

#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <qt/b3assetsenddialog.h>
#include <qt/b3theme.h>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <stdexcept>

namespace {
QLabel* Label(const QString& text, QWidget* parent)
{
    auto* label{new QLabel{text, parent}};
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}
QString Error(const UniValue& error)
{
    if (error.isObject() && error.find_value("message").isStr()) {
        return QString::fromStdString(error.find_value("message").get_str()).left(500);
    }
    return QStringLiteral("FN operator request failed.");
}
QString FinalityText(const UniValue& value)
{
    const auto& signing{value.find_value("staking")};
    if (!signing.isObject() || !signing.find_value("finality_signing").isBool() ||
        !signing.find_value("last_signed_height").isNum()) return QStringLiteral("B3 staking finality (node-wide): status unavailable");
    const auto& error{signing.find_value("last_error")};
    QString text{signing.find_value("finality_signing").get_bool() ? QStringLiteral("B3 staking finality (node-wide): keys armed") : QStringLiteral("B3 staking finality (node-wide): not armed")};
    text += QStringLiteral("; last signed checkpoint: %1").arg(signing.find_value("last_signed_height").getInt<int64_t>());
    if (error.isStr() && !error.get_str().empty()) text += QStringLiteral("\n") + QString::fromStdString(error.get_str()).left(500);
    return text;
}
} // namespace

struct B3FlowMeshPanel::Result {
    Operation operation{Operation::Refresh};
    std::optional<B3FlowMeshOperator::Status> state;
    QString finality;
    QString error;
    bool attempted{false};
};

B3FlowMeshPanel::B3FlowMeshPanel(QWidget* parent) : QWidget{parent}
{
    setObjectName(QStringLiteral("fnOperatorPanel"));
    B3Theme::markCard(this);
    auto* layout{new QVBoxLayout{this}};
    layout->setContentsMargins(20, 20, 20, 20);
    auto* heading{Label(tr("FN validator"), this)};
    B3Theme::markTextRole(heading, QStringLiteral("h2"));
    layout->addWidget(heading);
    layout->addWidget(Label(tr("FN keys validate FlowMesh markets. They are separate from B3 staking/finality keys. Arming loads keys; it does not prove a signature, an active seat or a finalized market checkpoint."), this));
    m_wallet_label = Label(tr("No wallet selected"), this);
    m_wallet_label->setObjectName(QStringLiteral("fnOperatorWallet"));
    m_armed = Label(tr("FN worker: status unavailable"), this);
    m_armed->setObjectName(QStringLiteral("fnArmedStatus"));
    m_finality = Label(tr("B3 staking finality: status unavailable"), this);
    m_finality->setObjectName(QStringLiteral("fnPosFinalityStatus"));
    layout->addWidget(m_wallet_label);
    layout->addWidget(m_armed);
    layout->addWidget(m_finality);
    layout->addWidget(Label(tr("This wallet's public FN keys"), this));
    m_keys = new QPlainTextEdit{this};
    m_keys->setObjectName(QStringLiteral("fnPublicKeys"));
    m_keys->setReadOnly(true);
    m_keys->setMinimumHeight(65);
    m_keys->setMaximumHeight(150);
    layout->addWidget(m_keys);
    layout->addWidget(Label(tr("Observed FlowMesh markets (node-wide)"), this));
    m_markets = new QPlainTextEdit{this};
    m_markets->setObjectName(QStringLiteral("fnMarketStatus"));
    m_markets->setReadOnly(true);
    m_markets->setMinimumHeight(65);
    m_markets->setMaximumHeight(150);
    layout->addWidget(m_markets);
    auto* actions{new QHBoxLayout};
    m_bind = new QPushButton{tr("Bind FN key…"), this};
    m_bind->setObjectName(QStringLiteral("fnBindKey"));
    m_arm = new QPushButton{tr("Arm this wallet's FN keys…"), this};
    m_arm->setObjectName(QStringLiteral("fnArmKeys"));
    m_disarm = new QPushButton{tr("Disarm all FN keys…"), this};
    m_disarm->setObjectName(QStringLiteral("fnDisarmKeys"));
    m_refresh = new QPushButton{tr("Refresh"), this};
    m_refresh->setObjectName(QStringLiteral("fnRefresh"));
    for (auto* button : {m_bind, m_arm, m_disarm, m_refresh}) {
        button->setAutoDefault(false);
        actions->addWidget(button);
    }
    layout->addLayout(actions);
    m_message = Label(tr("Bind uses one existing unbound FN, keeps it owned by this wallet and charges a B3 network fee. No ETH gas. Start/Stop affects the single FN worker shared by every wallet on this node."), this);
    m_message->setObjectName(QStringLiteral("fnOperatorMessage"));
    layout->addWidget(m_message);
    connect(m_bind, &QPushButton::clicked, this, &B3FlowMeshPanel::bind);
    connect(m_arm, &QPushButton::clicked, this, [this] { control(Operation::Arm); });
    connect(m_disarm, &QPushButton::clicked, this, [this] { control(Operation::Disarm); });
    connect(m_refresh, &QPushButton::clicked, this, &B3FlowMeshPanel::refresh);
    m_timer = new QTimer{this};
    m_timer->setInterval(15000);
    connect(m_timer, &QTimer::timeout, this, [this] { if (isVisible()) refresh(); });
    connect(qApp, &QCoreApplication::aboutToQuit, this, &B3FlowMeshPanel::cancelAndWait);
    updateControls();
}

B3FlowMeshPanel::~B3FlowMeshPanel() { cancelAndWait(); }

void B3FlowMeshPanel::setWalletModel(WalletModel* wallet)
{
    cancelAndWait();
    if (m_wallet) disconnect(m_wallet, nullptr, this, nullptr);
    m_wallet = wallet;
    m_backend.reset();
    m_state.reset();
    m_fn = {};
    m_keys->clear();
    m_markets->clear();
    m_finality->setText(tr("B3 staking finality: status unavailable"));
    if (wallet) {
        for (auto& candidate : wallet->node().walletLoader().getWallets()) {
            if (candidate->wallet() == wallet->wallet().wallet()) { m_backend = std::move(candidate); break; }
        }
        connect(wallet, &WalletModel::unload, this, [this] { setWalletModel(nullptr); });
        connect(wallet, &QObject::destroyed, this, [this] { setWalletModel(nullptr); });
        m_wallet_label->setText(tr("Selected wallet: %1").arg(wallet->getDisplayName()));
        m_timer->start();
    } else {
        m_wallet_label->setText(tr("No wallet selected"));
    }
    m_cancel->store(false);
    showStatus();
    refresh();
}

void B3FlowMeshPanel::setFnAsset(const B3AssetRecord& asset) { m_fn = asset; updateControls(); }

void B3FlowMeshPanel::updateControls()
{
    const bool ready{m_wallet && m_backend && !m_busy && !m_thread && m_security_warning.isEmpty()};
    m_refresh->setEnabled(ready);
    m_bind->setEnabled(ready && !m_backend->privateKeysDisabled() && m_fn.is_fn &&
                       m_fn.status == B3AssetRecord::Status::Active && m_fn.confirmed >= 1);
    m_bind->setToolTip(tr("Requires a loaded spending wallet and a confirmed unbound FN. The backend checks eligibility before preparing; this does not create or burn an FN."));
    m_arm->setEnabled(ready && m_state && m_state->available &&
                     !m_backend->privateKeysDisabled() && !m_state->wallet_keys.empty());
    m_disarm->setEnabled(ready && m_state && m_state->available && m_state->armed);
}

void B3FlowMeshPanel::showStatus()
{
    if (!m_security_warning.isEmpty()) m_message->setText(m_security_warning);
    if (!m_state) {
        m_armed->setText(tr("FN worker: status unavailable"));
    } else {
        m_armed->setText(tr("FN worker: %1 — %2 armed keys node-wide; %3 of this wallet's %4 keys loaded. This is not proof of signing.")
            .arg(m_state->armed ? tr("armed") : tr("not armed"))
            .arg(m_state->armed_keys.size()).arg(m_state->wallet_armed).arg(m_state->wallet_keys.size()));
        QStringList keys;
        for (const auto& key : m_state->wallet_keys) {
            keys.append(key + (m_state->armed_keys.contains(key) ? tr(" — loaded") : tr(" — not loaded")));
        }
        m_keys->setPlainText(keys.empty() ? tr("No stored FN BLS public keys in this wallet. Bind an existing FN first.") : keys.join(QLatin1Char('\n')));
        m_markets->setPlainText(m_state->markets.empty() ? tr("No FlowMesh markets reported. Armed keys alone cannot produce market signatures.") : m_state->markets.join(QLatin1Char('\n')));
    }
    updateControls();
}

void B3FlowMeshPanel::refresh()
{
    if (!m_wallet || !m_backend || m_busy || m_thread) return;
    startJob(Operation::Refresh);
}

void B3FlowMeshPanel::control(Operation operation)
{
    if (!m_wallet || !m_backend || !m_state || m_busy || m_thread) return;
    const auto state{*m_state};
    const QPointer<B3FlowMeshPanel> self{this};
    const QPointer<WalletModel> wallet{m_wallet};
    const auto generation{m_generation};
    m_busy = true;
    updateControls();
    const QString question{operation == Operation::Arm
        ? tr("Load %1 FN keys from wallet '%2'?\n\nThis REPLACES the node-wide FN key set (%3 keys currently loaded), including keys used by other wallets. It does not start B3 staking or prove market eligibility. The wallet returns to its previous spending-lock state after the operation.")
            .arg(state.wallet_keys.size()).arg(wallet->getDisplayName()).arg(state.armed_keys.size())
        : tr("Disarm ALL %1 FN keys on this node? This affects every wallet using the shared FN worker. It does not stop B3 staking/finality.").arg(state.armed_keys.size())};
    m_confirmation = new QMessageBox{QMessageBox::Warning, tr("Node-wide FN operation"), question,
        QMessageBox::Yes | QMessageBox::Cancel, this};
    const QPointer<QMessageBox> confirmation{m_confirmation};
    confirmation->setDefaultButton(QMessageBox::Cancel);
    confirmation->setTextFormat(Qt::PlainText);
    const bool accepted{confirmation->exec() == QMessageBox::Yes};
    if (confirmation) delete confirmation.data();
    if (!self || !wallet || generation != m_generation) return;
    m_confirmation = nullptr;
    if (!accepted) { m_busy = false; updateControls(); return; }
    if (operation == Operation::Arm) {
        const bool was_locked{m_backend->isLocked()};
        m_restore_locked = was_locked;
        m_relock_backend = was_locked ? m_backend : nullptr;
        m_relock_wallet_name = wallet->getDisplayName();
        std::unique_ptr<WalletModel::UnlockContext> unlock;
        try {
            unlock.reset(new WalletModel::UnlockContext{wallet->requestUnlock(WalletModel::UnlockPurpose::General)});
        } catch (...) {
            if (wallet && was_locked) wallet->setWalletLocked(true);
            if (self && generation == m_generation) {
                restoreSpendingLock();
                m_busy = false;
                m_message->setText(tr("Unlock failed; no FN arming request was made."));
                showStatus();
            }
            return;
        }
        if (!self || !wallet || generation != m_generation) return;
        if (!unlock->isValid() || m_backend->isLocked()) {
            unlock.reset();
            restoreSpendingLock();
            m_busy = false;
            m_message->setText(tr("Wallet not unlocked; no FN arming request was made."));
            showStatus();
            return;
        }
        m_unlock = std::move(unlock);
    }
    m_message->setText(tr("Applying the confirmed FN key change once. A changed node-wide key set will reject this request."));
    startJob(operation, B3FlowMeshOperator::ControlParameters(state));
}

void B3FlowMeshPanel::bind()
{
    if (!m_wallet || !m_backend || m_busy || m_thread || !m_fn.is_fn) return;
    m_busy = true;
    updateControls();
    const QPointer<B3FlowMeshPanel> self{this};
    const auto generation{m_generation};
    m_binding_dialog = new B3AssetSendDialog{m_wallet, m_fn, this, true};
    connect(m_binding_dialog, &B3AssetSendDialog::securityWarning, this, [this](const QString& warning) {
        m_security_warning = warning;
        showStatus();
    });
    QPointer<B3AssetSendDialog> dialog{m_binding_dialog};
    dialog->exec();
    if (dialog) delete dialog.data();
    if (!self || generation != m_generation) return;
    m_binding_dialog = nullptr;
    m_busy = false;
    m_message->setText(tr("If binding was prepared, make a fresh wallet backup for its new BLS key. Check the binding transaction's confirmation before expecting seat eligibility."));
    showStatus();
    refresh();
}

void B3FlowMeshPanel::startJob(Operation operation, const UniValue& params)
{
    if (!m_wallet || !m_backend || m_thread) return;
    m_busy = true;
    m_cancel->store(false);
    updateControls();
    const auto result{std::make_shared<Result>()};
    result->operation = operation;
    auto* node{&m_wallet->node()};
    const auto backend{m_backend};
    const auto cancel{m_cancel};
    const auto uri{B3AssetTransfer::WalletUri(m_wallet->getWalletName())};
    const auto generation{m_generation};
    m_thread = QThread::create([node, backend, cancel, uri, operation, params, result] {
        try {
            if (cancel->load() || node->shutdownRequested()) return;
            if (operation != Operation::Refresh) {
                if (operation == Operation::Arm && (backend->isLocked() || backend->privateKeysDisabled())) {
                    throw std::runtime_error{"The captured wallet is no longer unlocked for FN arming."};
                }
                if (cancel->load()) return;
                result->attempted = true;
                node->executeRpc(operation == Operation::Arm ? "startflowmeshvalidator" : "stopflowmeshvalidator", params, uri);
            }
            result->state = B3FlowMeshOperator::ParseStatus(node->executeRpc("getflowmeshvalidatorinfo", UniValue{UniValue::VARR}, uri));
            // getfinalityinfo may create a PoS key. Refresh must remain read-only.
            try { result->finality = FinalityText(node->executeRpc("getstakinginfo", UniValue{UniValue::VARR}, uri)); }
            catch (...) { result->finality = QStringLiteral("B3 staking finality: status unavailable"); }
        } catch (const UniValue& error) { result->error = Error(error); }
        catch (const std::exception& error) { result->error = QString::fromUtf8(error.what()).left(500); }
        catch (...) { result->error = QStringLiteral("FN operator request failed."); }
    });
    m_thread->setParent(this);
    connect(m_thread, &QThread::finished, this, [this, result, generation] {
        if (generation == m_generation) finishJob(result);
    });
    m_thread->start();
}

void B3FlowMeshPanel::finishJob(const std::shared_ptr<Result>& result)
{
    stopWorker();
    restoreSpendingLock();
    m_busy = false;
    if (!m_wallet || m_cancel->load()) return;
    if (!result->error.isEmpty()) {
        m_state.reset();
        m_keys->clear();
        m_markets->clear();
        m_finality->setText(tr("B3 staking finality: status unavailable"));
        m_message->setText(result->attempted
            ? tr("FN request returned an error or its result could not be verified: %1\nRefresh before another action. No automatic retry was made.").arg(result->error)
            : result->error);
    } else {
        m_state = result->state;
        m_finality->setText(result->finality);
        if (result->operation != Operation::Refresh) m_message->setText(tr("FN key state refreshed. Keys loaded is not proof of a signature or certificate; check market progress separately."));
    }
    showStatus();
}

void B3FlowMeshPanel::stopWorker()
{
    if (!m_thread) return;
    disconnect(m_thread, nullptr, this, nullptr);
    m_thread->wait(); // No worker waits for UI callbacks.
    delete m_thread;
    m_thread = nullptr;
}

void B3FlowMeshPanel::cancelAndWait()
{
    if (m_timer) m_timer->stop();
    m_cancel->store(true);
    ++m_generation;
    if (m_confirmation) m_confirmation->reject();
    if (m_binding_dialog) m_binding_dialog->cancelAndWait();
    stopWorker();
    restoreSpendingLock();
    m_busy = false;
}

bool B3FlowMeshPanel::restoreSpendingLock()
{
    m_unlock.reset();
    if (!m_restore_locked) return true;
    try {
        if (m_relock_backend && !m_relock_backend->isLocked()) m_relock_backend->lock();
        if (m_relock_backend && m_relock_backend->isLocked()) {
            m_restore_locked = false;
            m_relock_backend.reset();
            return true;
        }
    } catch (...) { /* Preserve a visible warning rather than claiming success. */ }
    m_security_warning = tr("SECURITY: spending relock could not be verified for wallet '%1'. Lock that wallet or close the application before leaving it unattended. FN controls are disabled.")
        .arg(m_relock_wallet_name);
    m_message->setText(m_security_warning);
    qCritical("FN operator could not verify restoration of the wallet spending lock.");
    return false;
}
