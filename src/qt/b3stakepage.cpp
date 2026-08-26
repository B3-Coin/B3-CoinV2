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
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QResizeEvent>
#include <QScrollArea>
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

    auto* eyebrow = new QLabel(tr("NETWORK PARTICIPATION"), content);
    B3Theme::markTextRole(eyebrow, QStringLiteral("eyebrow"));
    layout->addWidget(eyebrow);
    auto* heading = new QLabel(tr("Stake"), content);
    B3Theme::markTextRole(heading, QStringLiteral("h1"));
    layout->addWidget(heading);
    auto* introduction = new QLabel(
        tr("Wallet-generated rewards are shown from real transaction history. Unavailable staking metrics remain explicitly unavailable."),
        content);
    introduction->setWordWrap(true);
    B3Theme::markTextRole(introduction, QStringLiteral("secondary"));
    layout->addWidget(introduction);

    m_no_wallet = new QLabel(tr("No wallet is loaded."), content);
    B3Theme::markTextRole(m_no_wallet, QStringLiteral("status"));
    layout->addWidget(m_no_wallet);

    m_card_grid = new QGridLayout();
    m_card_grid->setContentsMargins(0, 0, 0, 0);
    m_card_grid->setHorizontalSpacing(B3Theme::kSpaceMd);
    m_card_grid->setVerticalSpacing(B3Theme::kSpaceMd);

    QVBoxLayout* statusLayout{nullptr};
    m_status_card = MakeCard(content, tr("Staking status"), &statusLayout);
    m_status_card->setObjectName(QStringLiteral("stakeStatusCard"));
    m_status_card->setProperty("b3surface", QStringLiteral("quiet"));
    {
        m_backend_state = new QLabel(
            tr("No staking backend is exposed to the wallet UI in this "
               "build. Activation, eligible balance, network weight and "
               "expected time will appear here when the wallet provides a "
               "staking model."),
            m_status_card);
        m_backend_state->setWordWrap(true);
        B3Theme::markTextRole(m_backend_state, QStringLiteral("secondary"));
        statusLayout->addWidget(m_backend_state);
        AddInfoRow(statusLayout, m_status_card, tr("Wallet lock state"), &m_lock_state);
        statusLayout->addStretch();
    }

    QVBoxLayout* balanceLayout{nullptr};
    m_balance_card = MakeCard(content, tr("Wallet signals"), &balanceLayout);
    m_balance_card->setObjectName(QStringLiteral("stakeBalanceCard"));
    m_balance_card->setProperty("b3surface", QStringLiteral("panel"));
    {
        AddInfoRow(balanceLayout, m_balance_card, tr("Wallet balance"), &m_wallet_balance);
        B3Theme::markTextRole(m_wallet_balance, QStringLiteral("title"));
        QLabel* eligible{nullptr};
        AddInfoRow(balanceLayout, m_balance_card, tr("Eligible for staking"), &eligible);
        eligible->setText(tr("Not available"));
        QLabel* weight{nullptr};
        AddInfoRow(balanceLayout, m_balance_card, tr("Network weight"), &weight);
        weight->setText(tr("Not available"));
        balanceLayout->addStretch();
    }

    QVBoxLayout* rewardsLayout{nullptr};
    m_rewards_card = MakeCard(content, tr("Recent rewards"), &rewardsLayout);
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
        rewardsLayout->addWidget(m_rewards, 1);

        m_rewards_empty = new QLabel(tr("No reward transactions yet."), m_rewards_card);
        B3Theme::markTextRole(m_rewards_empty, QStringLiteral("secondary"));
        m_rewards_empty->setAlignment(Qt::AlignCenter);
        m_rewards_empty->setMinimumHeight(96);
        rewardsLayout->addWidget(m_rewards_empty);
    }
    layout->addLayout(m_card_grid, 1);
    scroll->setWidget(content);
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
    const int columns = width < 820 ? 1 : 2;
    if (columns == m_layout_columns) return;
    m_layout_columns = columns;
    for (QWidget* card : {m_status_card, m_balance_card, m_rewards_card}) {
        m_card_grid->removeWidget(card);
    }
    m_card_grid->setColumnStretch(0, 1);
    m_card_grid->setColumnStretch(1, columns == 2 ? 1 : 0);
    if (columns == 1) {
        m_card_grid->addWidget(m_status_card, 0, 0);
        m_card_grid->addWidget(m_balance_card, 1, 0);
        m_card_grid->addWidget(m_rewards_card, 2, 0);
    } else {
        m_card_grid->addWidget(m_status_card, 0, 0);
        m_card_grid->addWidget(m_balance_card, 0, 1);
        m_card_grid->addWidget(m_rewards_card, 1, 0, 1, 2);
    }
}

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
