// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/b3stakepage.h>

#include <qt/b3theme.h>
#include <qt/bitcoinunits.h>
#include <qt/optionsmodel.h>
#include <qt/transactionfilterproxy.h>
#include <qt/transactionrecord.h>
#include <qt/transactiontablemodel.h>
#include <qt/walletmodel.h>

#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QTableView>
#include <QVBoxLayout>

namespace {
QWidget* MakeCard(QWidget* parent, const QString& title, QVBoxLayout** layout_out)
{
    auto* card = new QFrame(parent);
    B3Theme::markCard(card);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(B3Theme::kSpaceMd, B3Theme::kSpaceMd, B3Theme::kSpaceMd, B3Theme::kSpaceMd);
    layout->setSpacing(B3Theme::kSpaceSm);
    auto* title_label = new QLabel(title, card);
    B3Theme::markTextRole(title_label, QStringLiteral("h3"));
    layout->addWidget(title_label);
    *layout_out = layout;
    return card;
}

void AddInfoRow(QVBoxLayout* layout, QWidget* parent, const QString& title, QLabel** value_out)
{
    auto* row = new QHBoxLayout();
    auto* t = new QLabel(title, parent);
    B3Theme::markTextRole(t, QStringLiteral("secondary"));
    row->addWidget(t);
    row->addStretch();
    *value_out = new QLabel(QStringLiteral("—"), parent);
    (*value_out)->setTextInteractionFlags(Qt::TextSelectableByMouse);
    row->addWidget(*value_out);
    layout->addLayout(row);
}
} // namespace

B3StakePage::B3StakePage(QWidget* parent)
    : QWidget{parent}
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(B3Theme::kSpaceLg, B3Theme::kSpaceLg, B3Theme::kSpaceLg, B3Theme::kSpaceLg);
    layout->setSpacing(B3Theme::kSpaceMd);

    auto* heading = new QLabel(tr("Stake"), this);
    B3Theme::markTextRole(heading, QStringLiteral("h1"));
    layout->addWidget(heading);

    m_no_wallet = new QLabel(tr("No wallet is loaded."), this);
    B3Theme::markTextRole(m_no_wallet, QStringLiteral("secondary"));
    layout->addWidget(m_no_wallet);

    auto* row = new QHBoxLayout();
    row->setSpacing(B3Theme::kSpaceMd);

    QVBoxLayout* statusLayout{nullptr};
    QWidget* statusCard = MakeCard(this, tr("Status"), &statusLayout);
    {
        m_backend_state = new QLabel(
            tr("No staking backend is exposed to the wallet UI in this "
               "build. Activation, eligible balance, network weight and "
               "expected time will appear here when the wallet provides a "
               "staking model."),
            statusCard);
        m_backend_state->setWordWrap(true);
        B3Theme::markTextRole(m_backend_state, QStringLiteral("secondary"));
        statusLayout->addWidget(m_backend_state);
        AddInfoRow(statusLayout, statusCard, tr("Wallet lock state"), &m_lock_state);
        statusLayout->addStretch();
    }
    row->addWidget(statusCard, 1);

    QVBoxLayout* balanceLayout{nullptr};
    QWidget* balanceCard = MakeCard(this, tr("Balances"), &balanceLayout);
    {
        AddInfoRow(balanceLayout, balanceCard, tr("Wallet balance"), &m_wallet_balance);
        QLabel* eligible{nullptr};
        AddInfoRow(balanceLayout, balanceCard, tr("Eligible for staking"), &eligible);
        eligible->setText(tr("Not available"));
        QLabel* weight{nullptr};
        AddInfoRow(balanceLayout, balanceCard, tr("Network weight"), &weight);
        weight->setText(tr("Not available"));
        balanceLayout->addStretch();
    }
    row->addWidget(balanceCard, 1);
    layout->addLayout(row);

    QVBoxLayout* rewardsLayout{nullptr};
    QWidget* rewardsCard = MakeCard(this, tr("Recent rewards"), &rewardsLayout);
    {
        m_rewards = new QTableView(rewardsCard);
        m_rewards->setObjectName(QStringLiteral("stakeRewards"));
        m_rewards->setShowGrid(false);
        m_rewards->verticalHeader()->setVisible(false);
        m_rewards->horizontalHeader()->setStretchLastSection(true);
        m_rewards->setFrameShape(QFrame::NoFrame);
        m_rewards->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_rewards->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_rewards->hide();
        rewardsLayout->addWidget(m_rewards, 1);

        m_rewards_empty = new QLabel(tr("No reward transactions yet."), rewardsCard);
        B3Theme::markTextRole(m_rewards_empty, QStringLiteral("secondary"));
        rewardsLayout->addWidget(m_rewards_empty);
    }
    layout->addWidget(rewardsCard, 1);

    setWalletModel(nullptr);
}

B3StakePage::~B3StakePage() = default;

void B3StakePage::setWalletModel(WalletModel* wallet_model)
{
    if (m_wallet_model) {
        disconnect(m_wallet_model, nullptr, this, nullptr);
    }
    m_rewards->setModel(nullptr);
    m_rewards_filter.reset();
    m_wallet_model = wallet_model;

    const bool have_wallet = m_wallet_model != nullptr;
    m_no_wallet->setVisible(!have_wallet);

    if (!have_wallet) {
        m_lock_state->setText(QStringLiteral("—"));
        m_wallet_balance->setText(QStringLiteral("—"));
        m_rewards->hide();
        m_rewards_empty->show();
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

    const auto refreshEmptyState = [this] {
        const bool have_rows = m_rewards_filter && m_rewards_filter->rowCount() > 0;
        m_rewards->setVisible(have_rows);
        m_rewards_empty->setVisible(!have_rows);
    };
    connect(m_rewards_filter.get(), &TransactionFilterProxy::rowsInserted, this, refreshEmptyState);
    connect(m_rewards_filter.get(), &TransactionFilterProxy::rowsRemoved, this, refreshEmptyState);
    connect(m_rewards_filter.get(), &TransactionFilterProxy::modelReset, this, refreshEmptyState);
    refreshEmptyState();

    connect(m_wallet_model, &WalletModel::balanceChanged, this, &B3StakePage::setBalance);
    connect(m_wallet_model, &WalletModel::encryptionStatusChanged, this, &B3StakePage::updateLockState);

    const auto& balances = m_wallet_model->getCachedBalance();
    if (balances.balance != -1) setBalance(balances);
    updateLockState();
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
        m_lock_state->setText(tr("Locked"));
        break;
    case WalletModel::Unlocked:
        m_lock_state->setText(tr("Unlocked"));
        break;
    }
}
