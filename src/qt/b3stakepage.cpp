// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/b3stakepage.h>

#include <qt/b3theme.h>
#include <qt/bitcoinamountfield.h>
#include <qt/bitcoinunits.h>
#include <qt/optionsmodel.h>
#include <qt/transactionfilterproxy.h>
#include <qt/transactionrecord.h>
#include <qt/transactiontablemodel.h>
#include <qt/walletmodel.h>

#include <consensus/amount.h>

#include <QClipboard>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSizePolicy>
#include <QStyle>
#include <QTableView>
#include <QVBoxLayout>

namespace {
QWidget* MakeCard(QWidget* parent, const QString& title, QVBoxLayout** layout_out)
{
    auto* card = new QFrame(parent);
    B3Theme::markCard(card);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(B3Theme::kSpaceMd, B3Theme::kSpaceMd,
                               B3Theme::kSpaceMd, B3Theme::kSpaceMd);
    layout->setSpacing(B3Theme::kSpaceSm);
    auto* title_label = new QLabel(title, card);
    B3Theme::markTextRole(title_label, QStringLiteral("h3"));
    layout->addWidget(title_label);
    *layout_out = layout;
    return card;
}

void AddInfoRow(QVBoxLayout* layout, QWidget* parent, const QString& title,
                QLabel** value_out, const char* object_name = nullptr)
{
    auto* row = new QHBoxLayout();
    auto* title_label = new QLabel(title, parent);
    B3Theme::markTextRole(title_label, QStringLiteral("secondary"));
    row->addWidget(title_label);
    row->addStretch();
    *value_out = new QLabel(QStringLiteral("—"), parent);
    if (object_name) (*value_out)->setObjectName(QLatin1String(object_name));
    (*value_out)->setTextInteractionFlags(Qt::TextSelectableByMouse);
    (*value_out)->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row->addWidget(*value_out);
    layout->addLayout(row);
}

void AddKeyRow(QVBoxLayout* layout, QWidget* parent, const QString& title,
               QLabel** value_out, QPushButton** copy_out,
               const char* value_name, const char* copy_name)
{
    auto* row = new QHBoxLayout();
    auto* title_label = new QLabel(title, parent);
    B3Theme::markTextRole(title_label, QStringLiteral("secondary"));
    row->addWidget(title_label);
    row->addStretch();

    *value_out = new QLabel(QStringLiteral("—"), parent);
    (*value_out)->setObjectName(QLatin1String(value_name));
    (*value_out)->setTextInteractionFlags(Qt::TextSelectableByMouse);
    B3Theme::markTextRole(*value_out, QStringLiteral("mono"));
    row->addWidget(*value_out);

    *copy_out = new QPushButton(QObject::tr("Copy"), parent);
    (*copy_out)->setObjectName(QLatin1String(copy_name));
    (*copy_out)->setEnabled(false);
    row->addWidget(*copy_out);
    layout->addLayout(row);
}

QString ShortKey(const QString& key)
{
    if (key.size() <= 24) return key;
    return key.left(12) + QChar{0x2026} + key.right(10);
}

QString PhaseText(const QString& phase)
{
    if (phase == QLatin1String("legacy")) return QObject::tr("Legacy chain");
    if (phase == QLatin1String("corridor")) return QObject::tr("PoW transition corridor");
    if (phase == QLatin1String("modern_pos")) return QObject::tr("Modern proof of stake");
    if (phase == QLatin1String("modern")) return QObject::tr("Modern chain");
    return phase.isEmpty() ? QStringLiteral("—") : phase;
}

QString DetailString(const QVariantMap& details, const char* key)
{
    return details.value(QLatin1String(key)).toString();
}
} // namespace

B3StakePage::B3StakePage(QWidget* parent)
    : QWidget{parent},
      m_controller{new B3ValidatorController(this)}
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);

    auto* content = new QWidget(scroll);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(B3Theme::kSpaceLg, B3Theme::kSpaceLg,
                               B3Theme::kSpaceLg, B3Theme::kSpaceXl);
    layout->setSpacing(B3Theme::kSpaceLg);

    auto* eyebrow = new QLabel(tr("VALIDATOR OPERATIONS"), content);
    B3Theme::markTextRole(eyebrow, QStringLiteral("eyebrow"));
    layout->addWidget(eyebrow);
    auto* heading = new QLabel(tr("Stake & finality"), content);
    B3Theme::markTextRole(heading, QStringLiteral("h1"));
    layout->addWidget(heading);
    auto* introduction = new QLabel(
        tr("Create the wallet's public BLS binding, lock native B3 as stake, run Modern PoS, and explicitly help mine the temporary corridor. Private keys stay on this device and are never displayed."),
        content);
    introduction->setWordWrap(true);
    B3Theme::markTextRole(introduction, QStringLiteral("secondary"));
    layout->addWidget(introduction);

    m_no_wallet = new QLabel(tr("Load or select a descriptor wallet to configure a validator."), content);
    m_no_wallet->setObjectName(QStringLiteral("stakeNoWallet"));
    m_no_wallet->setWordWrap(true);
    B3Theme::markTextRole(m_no_wallet, QStringLiteral("status"));
    layout->addWidget(m_no_wallet);

    m_card_grid = new QGridLayout();
    m_card_grid->setContentsMargins(0, 0, 0, 0);
    m_card_grid->setHorizontalSpacing(B3Theme::kSpaceMd);
    m_card_grid->setVerticalSpacing(B3Theme::kSpaceMd);

    QVBoxLayout* status_layout{nullptr};
    m_status_card = MakeCard(content, tr("Validator status"), &status_layout);
    m_status_card->setObjectName(QStringLiteral("stakeStatusCard"));
    m_status_card->setProperty("b3surface", QStringLiteral("quiet"));
    {
        m_backend_state = new QLabel(tr("Select a wallet to load validator state."), m_status_card);
        m_backend_state->setObjectName(QStringLiteral("stakeBackendState"));
        m_backend_state->setWordWrap(true);
        B3Theme::markTextRole(m_backend_state, QStringLiteral("secondary"));
        status_layout->addWidget(m_backend_state);
        AddInfoRow(status_layout, m_status_card, tr("Wallet"), &m_lock_state, "stakeLockState");
        AddInfoRow(status_layout, m_status_card, tr("Chain height"), &m_chain_height, "stakeChainHeight");
        AddInfoRow(status_layout, m_status_card, tr("Next block"), &m_phase, "stakeNextPhase");
        AddKeyRow(status_layout, m_status_card, tr("Validator key"), &m_validator_key,
                  &m_copy_validator, "stakeValidatorKey", "stakeCopyValidator");
        AddKeyRow(status_layout, m_status_card, tr("BLS public key"), &m_bls_key,
                  &m_copy_bls, "stakeBlsKey", "stakeCopyBls");
        AddInfoRow(status_layout, m_status_card, tr("BLS binding"), &m_binding_state, "stakeBindingState");
        AddInfoRow(status_layout, m_status_card, tr("Validator set"), &m_set_state, "stakeSetState");
        AddInfoRow(status_layout, m_status_card, tr("Block producer"), &m_staking_state, "stakeProducerState");
        AddInfoRow(status_layout, m_status_card, tr("Finality signer"), &m_finality_state, "stakeFinalityState");
        status_layout->addStretch();
    }

    QVBoxLayout* balance_layout{nullptr};
    m_balance_card = MakeCard(content, tr("Stake balances"), &balance_layout);
    m_balance_card->setObjectName(QStringLiteral("stakeBalanceCard"));
    m_balance_card->setProperty("b3surface", QStringLiteral("panel"));
    {
        AddInfoRow(balance_layout, m_balance_card, tr("Wallet balance"), &m_wallet_balance, "stakeWalletBalance");
        B3Theme::markTextRole(m_wallet_balance, QStringLiteral("title"));
        AddInfoRow(balance_layout, m_balance_card, tr("Minimum stake"), &m_minimum_stake, "stakeMinimum");
        AddInfoRow(balance_layout, m_balance_card, tr("Active stake"), &m_active_stake, "stakeActive");
        AddInfoRow(balance_layout, m_balance_card, tr("Pending / unconfirmed"), &m_pending_stake, "stakePending");
        AddInfoRow(balance_layout, m_balance_card, tr("Next-block set weight / total"), &m_validator_weight, "stakeWeight");
        balance_layout->addStretch();
    }

    QVBoxLayout* controls_layout{nullptr};
    m_controls_card = MakeCard(content, tr("Validator controls"), &controls_layout);
    m_controls_card->setObjectName(QStringLiteral("stakeControlsCard"));
    m_controls_card->setProperty("b3surface", QStringLiteral("hero"));
    {
        auto* key_note = new QLabel(
            tr("The wallet generates the BLS key internally and publishes only its public binding. A small transaction fee is required."),
            m_controls_card);
        key_note->setWordWrap(true);
        B3Theme::markTextRole(key_note, QStringLiteral("secondary"));
        controls_layout->addWidget(key_note);

        auto* key_buttons = new QHBoxLayout();
        m_bind_finality = new QPushButton(tr("Generate & bind BLS key"), m_controls_card);
        m_bind_finality->setObjectName(QStringLiteral("stakeBindFinality"));
        m_bind_finality->setProperty("b3variant", QStringLiteral("primary"));
        key_buttons->addWidget(m_bind_finality);
        m_backup = new QPushButton(tr("Back up wallet"), m_controls_card);
        m_backup->setObjectName(QStringLiteral("stakeBackupWallet"));
        key_buttons->addWidget(m_backup);
        controls_layout->addLayout(key_buttons);

        auto* amount_title = new QLabel(tr("Amount to stake"), m_controls_card);
        B3Theme::markTextRole(amount_title, QStringLiteral("secondary"));
        controls_layout->addWidget(amount_title);
        auto* stake_row = new QHBoxLayout();
        m_stake_amount = new BitcoinAmountField(m_controls_card);
        m_stake_amount->setObjectName(QStringLiteral("stakeAmount"));
        m_stake_amount->setDisplayUnit(BitcoinUnit::BTC);
        m_stake_amount->SetAllowEmpty(true);
        stake_row->addWidget(m_stake_amount, 1);
        m_create_stake = new QPushButton(tr("Create stake"), m_controls_card);
        m_create_stake->setObjectName(QStringLiteral("stakeCreate"));
        stake_row->addWidget(m_create_stake);
        controls_layout->addLayout(stake_row);

        m_start_stop = new QPushButton(tr("Start staking"), m_controls_card);
        m_start_stop->setObjectName(QStringLiteral("stakeStartStop"));
        controls_layout->addWidget(m_start_stop);

        auto* mining_note = new QLabel(
            tr("Corridor mining is optional, uses one CPU thread, pays fees only, and stops automatically when Modern PoS begins."),
            m_controls_card);
        mining_note->setWordWrap(true);
        B3Theme::markTextRole(mining_note, QStringLiteral("secondary"));
        controls_layout->addWidget(mining_note);
        m_corridor_mining = new QPushButton(tr("Start corridor mining"), m_controls_card);
        m_corridor_mining->setObjectName(QStringLiteral("stakeCorridorMining"));
        controls_layout->addWidget(m_corridor_mining);

        m_operation_state = new QLabel(tr("No operation is running."), m_controls_card);
        m_operation_state->setObjectName(QStringLiteral("stakeOperationState"));
        m_operation_state->setWordWrap(true);
        m_operation_state->setTextInteractionFlags(Qt::TextSelectableByMouse);
        B3Theme::markTextRole(m_operation_state, QStringLiteral("secondary"));
        controls_layout->addWidget(m_operation_state);
        controls_layout->addStretch();
    }

    QVBoxLayout* rewards_layout{nullptr};
    m_rewards_card = MakeCard(content, tr("Recent block rewards"), &rewards_layout);
    m_rewards_card->setObjectName(QStringLiteral("stakeRewardsCard"));
    m_rewards_card->setProperty("b3surface", QStringLiteral("panel"));
    {
        m_rewards = new QTableView(m_rewards_card);
        m_rewards->setObjectName(QStringLiteral("stakeRewards"));
        m_rewards->setShowGrid(false);
        m_rewards->verticalHeader()->setVisible(false);
        m_rewards->horizontalHeader()->setStretchLastSection(true);
        m_rewards->setFrameShape(QFrame::NoFrame);
        m_rewards->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_rewards->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_rewards->setAlternatingRowColors(true);
        m_rewards->verticalHeader()->setDefaultSectionSize(44);
        m_rewards->setMinimumHeight(220);
        m_rewards->hide();
        rewards_layout->addWidget(m_rewards, 1);

        m_rewards_empty = new QLabel(tr("No reward transactions yet."), m_rewards_card);
        B3Theme::markTextRole(m_rewards_empty, QStringLiteral("secondary"));
        m_rewards_empty->setAlignment(Qt::AlignCenter);
        m_rewards_empty->setMinimumHeight(96);
        rewards_layout->addWidget(m_rewards_empty);
    }

    layout->addLayout(m_card_grid, 1);
    scroll->setWidget(content);

    connect(m_controller, &B3ValidatorController::statusChanged,
            this, &B3StakePage::setValidatorStatus);
    connect(m_controller, &B3ValidatorController::operationSucceeded,
            this, &B3StakePage::operationSucceeded);
    connect(m_controller, &B3ValidatorController::operationFailed,
            this, &B3StakePage::operationFailed);
    connect(m_bind_finality, &QPushButton::clicked, this, &B3StakePage::bindFinalityKey);
    connect(m_create_stake, &QPushButton::clicked, this, &B3StakePage::createStake);
    connect(m_start_stop, &QPushButton::clicked, this, &B3StakePage::toggleStaking);
    connect(m_corridor_mining, &QPushButton::clicked, this, &B3StakePage::toggleCorridorMining);
    connect(m_backup, &QPushButton::clicked, this, &B3StakePage::backupRequested);
    connect(m_copy_validator, &QPushButton::clicked, this, [this] {
        if (!m_validator_key_full.isEmpty()) QGuiApplication::clipboard()->setText(m_validator_key_full);
    });
    connect(m_copy_bls, &QPushButton::clicked, this, [this] {
        if (!m_bls_key_full.isEmpty()) QGuiApplication::clipboard()->setText(m_bls_key_full);
    });

    reflowCards(width());
    setWalletModel(nullptr);
}

B3StakePage::~B3StakePage() = default;

void B3StakePage::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    reflowCards(event->size().width());
}

void B3StakePage::reflowCards(int width)
{
    if (!m_card_grid || !m_rewards_card) return;
    const int columns = width < 900 ? 1 : 2;
    if (columns == m_layout_columns) return;
    m_layout_columns = columns;
    for (QWidget* card : {m_status_card, m_balance_card, m_controls_card, m_rewards_card}) {
        m_card_grid->removeWidget(card);
    }
    m_card_grid->setColumnStretch(0, 1);
    m_card_grid->setColumnStretch(1, columns == 2 ? 1 : 0);
    if (columns == 1) {
        m_card_grid->addWidget(m_status_card, 0, 0);
        m_card_grid->addWidget(m_balance_card, 1, 0);
        m_card_grid->addWidget(m_controls_card, 2, 0);
        m_card_grid->addWidget(m_rewards_card, 3, 0);
    } else {
        m_card_grid->addWidget(m_status_card, 0, 0);
        m_card_grid->addWidget(m_balance_card, 0, 1);
        m_card_grid->addWidget(m_controls_card, 1, 0);
        m_card_grid->addWidget(m_rewards_card, 1, 1);
    }
}

void B3StakePage::setWalletModel(WalletModel* wallet_model)
{
    if (m_wallet_model) disconnect(m_wallet_model, nullptr, this, nullptr);
    m_rewards->setModel(nullptr);
    m_rewards_filter.reset();
    m_wallet_model = wallet_model;
    m_status = {};
    m_operation_busy = false;
    m_stake_amount_initialized = false;
    resetValidatorDisplay();
    m_wallet_balance->setText(QStringLiteral("—"));

    const bool have_wallet = m_wallet_model != nullptr;
    m_no_wallet->setVisible(!have_wallet);
    m_controller->setWalletModel(wallet_model);

    if (!have_wallet) {
        m_lock_state->setText(QStringLiteral("—"));
        m_wallet_balance->setText(QStringLiteral("—"));
        m_backend_state->setText(tr("Select a wallet to load validator state."));
        m_rewards->hide();
        m_rewards_empty->show();
        setOperationMessage(tr("No wallet is loaded."));
        updateControls();
        return;
    }

    // Real reward history only: the wallet's own Generated transactions.
    m_rewards_filter = std::make_unique<TransactionFilterProxy>();
    m_rewards_filter->setSourceModel(m_wallet_model->getTransactionTableModel());
    m_rewards_filter->setTypeFilter(TransactionFilterProxy::TYPE(TransactionRecord::Generated));
    m_rewards_filter->setDynamicSortFilter(true);
    m_rewards_filter->setSortRole(Qt::EditRole);
    m_rewards_filter->sort(TransactionTableModel::Date, Qt::DescendingOrder);
    m_rewards->setModel(m_rewards_filter.get());
    for (int column : {TransactionTableModel::Status, TransactionTableModel::ToAddress}) {
        m_rewards->setColumnHidden(column, true);
    }

    const auto refresh_empty_state = [this] {
        const bool have_rows = m_rewards_filter && m_rewards_filter->rowCount() > 0;
        m_rewards->setVisible(have_rows);
        m_rewards_empty->setVisible(!have_rows);
    };
    connect(m_rewards_filter.get(), &TransactionFilterProxy::rowsInserted, this, refresh_empty_state);
    connect(m_rewards_filter.get(), &TransactionFilterProxy::rowsRemoved, this, refresh_empty_state);
    connect(m_rewards_filter.get(), &TransactionFilterProxy::modelReset, this, refresh_empty_state);
    refresh_empty_state();

    connect(m_wallet_model, &WalletModel::balanceChanged, this, &B3StakePage::setBalance);
    connect(m_wallet_model, &WalletModel::encryptionStatusChanged, this, &B3StakePage::updateLockState);

    const auto& balances = m_wallet_model->getCachedBalance();
    if (balances.balance != -1) setBalance(balances);
    updateLockState();
    setOperationMessage(tr("Loading validator state…"));
    updateControls();
}

void B3StakePage::setBalance(const interfaces::WalletBalances& balances)
{
    if (!m_wallet_model || !m_wallet_model->getOptionsModel()) return;
    const BitcoinUnit unit = m_wallet_model->getOptionsModel()->getDisplayUnit();
    m_wallet_balance->setText(BitcoinUnits::formatWithUnit(unit, balances.balance));
}

void B3StakePage::updateLockState()
{
    if (!m_wallet_model) return;
    switch (m_wallet_model->getEncryptionStatus()) {
    case WalletModel::NoKeys:
        m_lock_state->setText(tr("Watch-only"));
        break;
    case WalletModel::Unencrypted:
        m_lock_state->setText(tr("Not encrypted"));
        break;
    case WalletModel::Locked:
        m_lock_state->setText(tr("Locked — actions will request the password"));
        break;
    case WalletModel::Unlocked:
        m_lock_state->setText(tr("Unlocked"));
        break;
    }
}

void B3StakePage::setValidatorStatus(const B3ValidatorStatus& status)
{
    m_status = status;
    if (!m_wallet_model) return;

    if (!status.valid) {
        resetValidatorDisplay();
        m_backend_state->setText(status.refresh_error.isEmpty()
                                     ? tr("Loading validator state…")
                                     : tr("Validator state is unavailable: %1").arg(status.refresh_error));
        updateControls();
        return;
    }

    m_chain_height->setText(QString::number(status.tip_height));
    m_phase->setText(PhaseText(status.next_block_phase));
    if (status.next_block_phase == QLatin1String("legacy")) {
        m_backend_state->setText(tr("Waiting for the pinned transition boundary. Validator transactions remain disabled."));
    } else if (status.next_block_phase == QLatin1String("corridor")) {
        m_backend_state->setText(tr("The transition corridor is active. Bind the BLS key, create stake early, and optionally help produce corridor blocks."));
    } else if (status.next_block_phase == QLatin1String("modern_pos")) {
        m_backend_state->setText(tr("Modern proof of stake is active. Set weights are frozen per epoch; newly active stake enters through a future certified rotation."));
    } else {
        m_backend_state->setText(tr("Validator backend connected."));
    }
    if (!status.refresh_error.isEmpty()) {
        m_backend_state->setText(m_backend_state->text() + QLatin1Char(' ') + status.refresh_error);
    }

    setPublicKeys(status.validator_key, status.bls_pubkey);
    if (!status.refresh_error.isEmpty()) {
        m_binding_state->setText(tr("Unavailable — retrying"));
    } else if (status.binding_pending) {
        m_binding_state->setText(tr("Submitted — awaiting confirmation"));
    } else if (status.finality_bound && !status.finality_revoked) {
        m_binding_state->setText(tr("Confirmed"));
    } else if (status.finality_revoked) {
        m_binding_state->setText(tr("Revoked"));
    } else {
        m_binding_state->setText(tr("Not bound"));
    }
    m_set_state->setText(status.current_set_member
                             ? tr("Current epoch member — frozen weight %1").arg(status.member_weight)
                             : tr("Not in current set"));
    if (status.staking_running && status.staking_uses_this_wallet) {
        m_staking_state->setText(tr("Running — %1").arg(status.staking_state));
    } else if (status.staking_running) {
        m_staking_state->setText(tr("Another wallet is staking"));
    } else {
        m_staking_state->setText(tr("Stopped"));
    }
    if (status.finality_signing && status.staking_uses_this_wallet) {
        m_finality_state->setText(tr("Armed — last signed %1").arg(status.last_signed_height));
    } else if (status.finality_signing) {
        m_finality_state->setText(tr("Armed by another wallet"));
    } else {
        m_finality_state->setText(tr("Not armed"));
    }

    const BitcoinUnit unit = m_wallet_model->getOptionsModel()
                                 ? m_wallet_model->getOptionsModel()->getDisplayUnit()
                                 : BitcoinUnit::BTC;
    m_minimum_stake->setText(status.min_stake_available
                                 ? BitcoinUnits::formatWithUnit(unit, status.min_stake_amount)
                                 : tr("Not configured"));
    m_active_stake->setText(BitcoinUnits::formatWithUnit(unit, status.active_stake));
    m_pending_stake->setText(
        tr("%1 / %2")
            .arg(BitcoinUnits::formatWithUnit(unit, status.pending_stake),
                 BitcoinUnits::formatWithUnit(unit, status.unconfirmed_stake)));
    m_validator_weight->setText(
        tr("%1 / %2").arg(status.eligible_weight).arg(status.total_eligible_weight));

    if (status.min_stake_available) {
        m_stake_amount->SetMinValue(status.min_stake_amount);
        bool amount_valid{false};
        const CAmount amount{m_stake_amount->value(&amount_valid)};
        if (!m_stake_amount_initialized || !amount_valid || amount < status.min_stake_amount) {
            m_stake_amount->setValue(status.min_stake_amount);
            m_stake_amount_initialized = true;
        }
    }

    if (status.auto_corridor_mining) {
        if (!status.corridor_mining_error.isEmpty()) {
            setOperationMessage(
                tr("Corridor mining is retrying after an error: %1").arg(status.corridor_mining_error),
                QStringLiteral("negative"));
        } else {
            setOperationMessage(status.corridor_mining_attempt
                                    ? tr("Mining the next corridor block on one CPU thread…")
                                    : tr("Corridor mining is armed; waiting for the next paced attempt."),
                                QStringLiteral("accent"));
        }
    }

    if (status.auto_corridor_mining) {
        Q_EMIT stakingSummaryChanged(tr("Mining corridor"));
    } else if (status.staking_running && status.staking_uses_this_wallet && status.finality_signing) {
        Q_EMIT stakingSummaryChanged(tr("Staking · finality armed"));
    } else if (status.staking_running && status.staking_uses_this_wallet) {
        Q_EMIT stakingSummaryChanged(tr("Staking"));
    } else if (status.finality_bound && status.active_stake > 0) {
        Q_EMIT stakingSummaryChanged(tr("Validator ready"));
    } else {
        Q_EMIT stakingSummaryChanged(QString{});
    }
    updateControls();
}

void B3StakePage::setPublicKeys(const QString& validator_key, const QString& bls_pubkey)
{
    if (!validator_key.isEmpty()) m_validator_key_full = validator_key;
    if (!bls_pubkey.isEmpty()) m_bls_key_full = bls_pubkey;
    if (validator_key.isEmpty() && !m_status.binding_pending) m_validator_key_full.clear();
    if (bls_pubkey.isEmpty() && !m_status.binding_pending) m_bls_key_full.clear();

    m_validator_key->setText(m_validator_key_full.isEmpty() ? QStringLiteral("—") : ShortKey(m_validator_key_full));
    m_validator_key->setToolTip(m_validator_key_full);
    m_bls_key->setText(m_bls_key_full.isEmpty() ? QStringLiteral("—") : ShortKey(m_bls_key_full));
    m_bls_key->setToolTip(m_bls_key_full);
    m_copy_validator->setEnabled(!m_validator_key_full.isEmpty());
    m_copy_bls->setEnabled(!m_bls_key_full.isEmpty());
}

void B3StakePage::resetValidatorDisplay()
{
    setPublicKeys({}, {});
    m_chain_height->setText(QStringLiteral("—"));
    m_phase->setText(QStringLiteral("—"));
    m_binding_state->setText(QStringLiteral("—"));
    m_set_state->setText(QStringLiteral("—"));
    m_staking_state->setText(QStringLiteral("—"));
    m_finality_state->setText(QStringLiteral("—"));
    m_minimum_stake->setText(QStringLiteral("—"));
    m_active_stake->setText(QStringLiteral("—"));
    m_pending_stake->setText(QStringLiteral("—"));
    m_validator_weight->setText(QStringLiteral("—"));
    Q_EMIT stakingSummaryChanged(QString{});
}

void B3StakePage::updateControls()
{
    const bool have_wallet{m_wallet_model != nullptr};
    const bool ready{have_wallet && m_status.valid && !m_operation_busy};
    const bool finality_ready{ready && m_status.refresh_error.isEmpty()};
    const bool modern_window{m_status.next_block_phase == QLatin1String("corridor") ||
                             m_status.next_block_phase == QLatin1String("modern_pos")};
    const bool can_prepare{ready && modern_window && m_status.min_stake_available};

    if (!m_status.refresh_error.isEmpty()) {
        m_bind_finality->setText(tr("BLS status unavailable"));
    } else if (m_status.binding_pending) {
        m_bind_finality->setText(tr("BLS binding submitted"));
    } else if (m_status.finality_bound && !m_status.finality_revoked) {
        m_bind_finality->setText(tr("BLS key bound"));
    } else {
        m_bind_finality->setText(tr("Generate & bind BLS key"));
    }
    m_bind_finality->setEnabled(can_prepare && finality_ready &&
                                (!m_status.finality_bound || m_status.finality_revoked) &&
                                !m_status.binding_pending);
    m_backup->setEnabled(have_wallet && !m_operation_busy);
    m_stake_amount->setEnabled(can_prepare);
    m_create_stake->setEnabled(can_prepare);

    m_start_stop->setText(m_status.staking_running
                              ? (m_status.staking_uses_this_wallet
                                     ? tr("Stop staking")
                                     : tr("Another wallet is staking"))
                              : tr("Start staking"));
    const bool have_any_stake{m_status.active_stake > 0 || m_status.pending_stake > 0 ||
                              m_status.unconfirmed_stake > 0};
    m_start_stop->setEnabled(
        ready && (m_status.staking_running
                      ? m_status.staking_uses_this_wallet
                      : (modern_window && finality_ready && m_status.finality_bound && have_any_stake)));

    m_corridor_mining->setText(m_status.auto_corridor_mining
                                   ? tr("Stop corridor mining")
                                   : tr("Start corridor mining"));
    m_corridor_mining->setEnabled(have_wallet && !m_operation_busy &&
                                  (m_status.auto_corridor_mining ||
                                   (m_status.valid && m_status.next_block_phase == QLatin1String("corridor"))));
}

void B3StakePage::bindFinalityKey()
{
    if (!m_wallet_model || !m_bind_finality->isEnabled()) return;
    const auto answer = QMessageBox::question(
        this, tr("Generate and bind BLS key"),
        tr("The wallet will create the BLS key internally and broadcast its public binding using a small transaction fee. Back up this wallet immediately afterward. Continue?"));
    if (answer != QMessageBox::Yes) return;
    m_operation_busy = true;
    setOperationMessage(tr("Creating the BLS binding…"), QStringLiteral("accent"));
    updateControls();
    m_controller->bindFinalityKey();
}

void B3StakePage::createStake()
{
    if (!m_wallet_model || !m_create_stake->isEnabled()) return;
    bool valid{false};
    const CAmount amount{m_stake_amount->value(&valid)};
    if (!valid || !m_stake_amount->validate() ||
        (m_status.min_stake_available && amount < m_status.min_stake_amount)) {
        setOperationMessage(tr("Enter an amount at or above the network minimum."), QStringLiteral("negative"));
        return;
    }
    const QString formatted{BitcoinUnits::formatWithUnit(BitcoinUnit::BTC, amount)};
    const auto answer = QMessageBox::question(
        this, tr("Create stake"),
        tr("Lock %1 as stake for this wallet's validator? The principal remains yours but is deliberately excluded from ordinary spending while staked.").arg(formatted));
    if (answer != QMessageBox::Yes) return;
    m_operation_busy = true;
    setOperationMessage(tr("Creating the stake transaction…"), QStringLiteral("accent"));
    updateControls();
    m_controller->createStake(amount);
}

void B3StakePage::toggleStaking()
{
    if (!m_wallet_model || !m_start_stop->isEnabled()) return;
    m_operation_busy = true;
    updateControls();
    if (m_status.staking_running) {
        setOperationMessage(tr("Stopping the validator and clearing in-memory signing keys…"), QStringLiteral("accent"));
        m_controller->stopStaking();
        return;
    }
    const auto answer = QMessageBox::question(
        this, tr("Start staking"),
        tr("Start the automatic Modern-PoS producer and finality signer? It continues after the wallet re-locks, until you stop it or close B3 Hive."));
    if (answer != QMessageBox::Yes) {
        m_operation_busy = false;
        updateControls();
        return;
    }
    setOperationMessage(tr("Starting the validator…"), QStringLiteral("accent"));
    m_controller->startStaking();
}

void B3StakePage::toggleCorridorMining()
{
    if (!m_wallet_model || !m_corridor_mining->isEnabled()) return;
    if (m_status.auto_corridor_mining) {
        m_controller->stopAutoCorridorMining();
        setOperationMessage(tr("Corridor mining will stop after the current bounded attempt."));
        return;
    }
    const auto answer = QMessageBox::question(
        this, tr("Start corridor mining"),
        tr("Use one CPU thread to produce explicitly paced transition blocks? Corridor blocks pay transaction fees only, and mining stops automatically at Modern PoS."));
    if (answer != QMessageBox::Yes) return;
    m_controller->startAutoCorridorMining();
}

void B3StakePage::operationSucceeded(const QString& operation, const QVariantMap& details)
{
    m_operation_busy = false;
    if (operation == QLatin1String("bind_finality_key")) {
        const QString txid{DetailString(details, "txid")};
        setPublicKeys(DetailString(details, "validator_key"), DetailString(details, "bls_pubkey"));
        setOperationMessage(
            tr("BLS binding submitted. Copy the public keys above and back up the wallet now. Transaction: %1").arg(txid),
            QStringLiteral("positive"));
    } else if (operation == QLatin1String("create_stake")) {
        setOperationMessage(tr("Stake transaction submitted: %1").arg(DetailString(details, "txid")),
                            QStringLiteral("positive"));
    } else if (operation == QLatin1String("start_staking")) {
        const bool armed{details.value(QStringLiteral("finality_signing")).toBool()};
        setOperationMessage(armed
                                ? tr("Staking started and the finality signer is armed.")
                                : tr("Staking started, but the finality signer is not armed. Check the confirmed BLS binding."),
                            armed ? QStringLiteral("positive") : QStringLiteral("negative"));
    } else if (operation == QLatin1String("stop_staking")) {
        setOperationMessage(tr("Staking stopped and in-memory signing keys were cleared."),
                            QStringLiteral("positive"));
    } else if (operation == QLatin1String("mine_corridor_block")) {
        setOperationMessage(tr("Corridor block mined: %1").arg(DetailString(details, "block_hash")),
                            QStringLiteral("positive"));
    } else if (operation == QLatin1String("start_corridor_mining")) {
        setOperationMessage(tr("Corridor mining started."), QStringLiteral("accent"));
    } else if (operation == QLatin1String("stop_corridor_mining")) {
        setOperationMessage(tr("Corridor mining stopped."));
    }
    updateControls();
}

void B3StakePage::operationFailed(const QString& operation, const QString& error)
{
    m_operation_busy = false;
    if (operation == QLatin1String("refresh")) {
        m_backend_state->setText(tr("Validator state is unavailable: %1").arg(error));
    } else {
        setOperationMessage(tr("%1 failed: %2").arg(operation, error), QStringLiteral("negative"));
        if (operation != QLatin1String("mine_corridor_block")) {
            QMessageBox::warning(this, tr("Validator operation failed"), error);
        }
    }
    updateControls();
}

void B3StakePage::setOperationMessage(const QString& message, const QString& role)
{
    m_operation_state->setText(message);
    B3Theme::markTextRole(m_operation_state, role);
    m_operation_state->style()->unpolish(m_operation_state);
    m_operation_state->style()->polish(m_operation_state);
}
