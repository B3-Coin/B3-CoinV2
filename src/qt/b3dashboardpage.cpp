// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/b3dashboardpage.h>

#include <qt/b3theme.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/transactionfilterproxy.h>
#include <qt/transactiontablemodel.h>
#include <qt/walletmodel.h>

#include <interfaces/node.h>

#include <QAbstractItemDelegate>
#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr int kActivityRows{5};
constexpr int kActivityRowHeight{48};

//! Compact two-line renderer for the recent-activity list. Presentation
//! only: every value shown comes from the wallet's transaction model.
class B3ActivityDelegate : public QAbstractItemDelegate
{
public:
    explicit B3ActivityDelegate(QObject* parent) : QAbstractItemDelegate(parent) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);

        const QRect rect = option.rect.adjusted(B3Theme::kSpaceSm, B3Theme::kSpaceXs, -B3Theme::kSpaceSm, -B3Theme::kSpaceXs);
        const int half = rect.height() / 2;
        const QRect topRect(rect.left(), rect.top(), rect.width(), half);
        const QRect bottomRect(rect.left(), rect.top() + half, rect.width(), rect.height() - half);

        const QString address = index.data(Qt::DisplayRole).toString();
        const QString type = index.sibling(index.row(), TransactionTableModel::Type).data().toString();
        const QDateTime date = index.data(TransactionTableModel::DateRole).toDateTime();
        const qint64 amount = index.data(TransactionTableModel::AmountRole).toLongLong();
        const bool confirmed = index.data(TransactionTableModel::ConfirmedRole).toBool();

        QString amount_text = BitcoinUnits::formatWithUnit(unit, amount, true, BitcoinUnits::SeparatorStyle::ALWAYS);
        if (!confirmed) amount_text = QStringLiteral("[%1]").arg(amount_text);
        const int amount_width = option.fontMetrics.horizontalAdvance(amount_text) + B3Theme::kSpaceMd;

        painter->setPen(amount < 0 ? B3Theme::kNegative : (confirmed ? B3Theme::kPositive : B3Theme::kTextMuted));
        painter->drawText(topRect, Qt::AlignRight | Qt::AlignVCenter, amount_text);

        painter->setPen(B3Theme::kTextPrimary);
        const QString primary = option.fontMetrics.elidedText(
            type.isEmpty() ? address : type + QStringLiteral(" · ") + address,
            Qt::ElideMiddle, topRect.width() - amount_width);
        painter->drawText(topRect, Qt::AlignLeft | Qt::AlignVCenter, primary);

        painter->setPen(B3Theme::kTextSecondary);
        painter->drawText(bottomRect, Qt::AlignLeft | Qt::AlignVCenter, GUIUtil::dateTimeStr(date));

        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override
    {
        return {0, kActivityRowHeight};
    }

    BitcoinUnit unit{BitcoinUnit::BTC};
};

QLabel* makeValueLabel(QWidget* parent)
{
    auto* label = new QLabel(QStringLiteral("—"), parent);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return label;
}

} // namespace

B3DashboardPage::B3DashboardPage(const PlatformStyle* platform_style, QWidget* parent)
    : QWidget{parent},
      m_platform_style{platform_style}
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);

    auto* content = new QWidget(scroll);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(B3Theme::kSpaceLg, B3Theme::kSpaceLg, B3Theme::kSpaceLg, B3Theme::kSpaceLg);
    layout->setSpacing(B3Theme::kSpaceMd);

    auto* heading = new QLabel(tr("Dashboard"), content);
    B3Theme::markTextRole(heading, QStringLiteral("h1"));
    layout->addWidget(heading);

    // Top row: balances + wallet/actions.
    auto* topRow = new QHBoxLayout();
    topRow->setSpacing(B3Theme::kSpaceMd);

    QWidget* balanceCard = makeCard(tr("Balances"));
    {
        auto* grid = new QGridLayout();
        grid->setHorizontalSpacing(B3Theme::kSpaceLg);
        grid->setVerticalSpacing(B3Theme::kSpaceSm);
        int row = 0;
        auto addRow = [&](const QString& title, QLabel** value_out, QLabel** title_out = nullptr) {
            auto* t = new QLabel(title, balanceCard);
            B3Theme::markTextRole(t, QStringLiteral("secondary"));
            auto* v = makeValueLabel(balanceCard);
            grid->addWidget(t, row, 0);
            grid->addWidget(v, row, 1);
            grid->setColumnStretch(1, 1);
            if (title_out) *title_out = t;
            *value_out = v;
            ++row;
        };
        addRow(tr("Available"), &m_available);
        B3Theme::markTextRole(m_available, QStringLiteral("h2"));
        addRow(tr("Pending"), &m_pending);
        addRow(tr("Immature"), &m_immature, &m_immature_title);
        addRow(tr("Total"), &m_total);
        static_cast<QVBoxLayout*>(balanceCard->layout())->addLayout(grid);
        static_cast<QVBoxLayout*>(balanceCard->layout())->addStretch();
    }
    topRow->addWidget(balanceCard, 3);

    QWidget* walletCard = makeCard(tr("Wallet"));
    {
        auto* cardLayout = static_cast<QVBoxLayout*>(walletCard->layout());
        m_wallet_name = new QLabel(walletCard);
        m_wallet_name->setTextInteractionFlags(Qt::TextSelectableByMouse);
        cardLayout->addWidget(m_wallet_name);

        m_security = new QLabel(walletCard);
        B3Theme::markTextRole(m_security, QStringLiteral("secondary"));
        cardLayout->addWidget(m_security);

        m_no_wallet = new QLabel(tr("No wallet is loaded."), walletCard);
        B3Theme::markTextRole(m_no_wallet, QStringLiteral("secondary"));
        m_no_wallet->setWordWrap(true);
        cardLayout->addWidget(m_no_wallet);

        cardLayout->addStretch();
        auto* buttons = new QHBoxLayout();
        m_send = new QPushButton(tr("Send"), walletCard);
        m_receive = new QPushButton(tr("Receive"), walletCard);
        m_send->setObjectName(QStringLiteral("dashboardSend"));
        m_receive->setObjectName(QStringLiteral("dashboardReceive"));
        buttons->addWidget(m_send);
        buttons->addWidget(m_receive);
        cardLayout->addLayout(buttons);

        connect(m_send, &QPushButton::clicked, this, &B3DashboardPage::sendRequested);
        connect(m_receive, &QPushButton::clicked, this, &B3DashboardPage::receiveRequested);
    }
    topRow->addWidget(walletCard, 2);
    layout->addLayout(topRow);

    // Middle row: staking + sync + network summaries.
    auto* midRow = new QHBoxLayout();
    midRow->setSpacing(B3Theme::kSpaceMd);

    QWidget* stakingCard = makeCard(tr("Staking"));
    {
        m_staking_note = new QLabel(
            tr("Staking information is not available in this build. It will "
               "appear here when the wallet exposes a staking model."),
            stakingCard);
        m_staking_note->setWordWrap(true);
        B3Theme::markTextRole(m_staking_note, QStringLiteral("secondary"));
        static_cast<QVBoxLayout*>(stakingCard->layout())->addWidget(m_staking_note);
        static_cast<QVBoxLayout*>(stakingCard->layout())->addStretch();
    }
    midRow->addWidget(stakingCard, 1);

    QWidget* syncCard = makeCard(tr("Synchronization"));
    {
        auto* cardLayout = static_cast<QVBoxLayout*>(syncCard->layout());
        m_sync_blocks = new QLabel(tr("Waiting for block data…"), syncCard);
        cardLayout->addWidget(m_sync_blocks);
        m_sync_time = new QLabel(syncCard);
        B3Theme::markTextRole(m_sync_time, QStringLiteral("secondary"));
        cardLayout->addWidget(m_sync_time);
        m_sync_progress = new QProgressBar(syncCard);
        m_sync_progress->setRange(0, 1000);
        m_sync_progress->setTextVisible(false);
        m_sync_progress->setFixedHeight(6);
        m_sync_progress->hide();
        cardLayout->addWidget(m_sync_progress);
        m_sync_warning = new QLabel(tr("Displayed information may be out of date."), syncCard);
        m_sync_warning->setWordWrap(true);
        B3Theme::markTextRole(m_sync_warning, QStringLiteral("secondary"));
        m_sync_warning->hide();
        cardLayout->addWidget(m_sync_warning);
        cardLayout->addStretch();
    }
    midRow->addWidget(syncCard, 1);

    QWidget* netCard = makeCard(tr("Network"));
    {
        auto* cardLayout = static_cast<QVBoxLayout*>(netCard->layout());
        m_net_peers = new QLabel(tr("No peer data yet."), netCard);
        cardLayout->addWidget(m_net_peers);
        m_net_state = new QLabel(netCard);
        B3Theme::markTextRole(m_net_state, QStringLiteral("secondary"));
        cardLayout->addWidget(m_net_state);
        cardLayout->addStretch();
    }
    midRow->addWidget(netCard, 1);
    layout->addLayout(midRow);

    // Recent activity.
    QWidget* activityCard = makeCard(tr("Recent activity"));
    {
        auto* cardLayout = static_cast<QVBoxLayout*>(activityCard->layout());
        m_activity = new QListView(activityCard);
        m_activity->setObjectName(QStringLiteral("dashboardActivity"));
        m_activity->setItemDelegate(new B3ActivityDelegate(m_activity));
        m_activity->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_activity->setFrameShape(QFrame::NoFrame);
        m_activity->setAttribute(Qt::WA_MacShowFocusRect, false);
        m_activity->setMinimumHeight(kActivityRows * kActivityRowHeight);
        m_activity->hide();
        cardLayout->addWidget(m_activity);

        m_activity_empty = new QLabel(tr("No transactions yet."), activityCard);
        B3Theme::markTextRole(m_activity_empty, QStringLiteral("secondary"));
        cardLayout->addWidget(m_activity_empty);

        m_activity_masked = new QLabel(tr("Values are masked. Uncheck Settings → Mask values to show activity."), activityCard);
        B3Theme::markTextRole(m_activity_masked, QStringLiteral("secondary"));
        m_activity_masked->setWordWrap(true);
        m_activity_masked->hide();
        cardLayout->addWidget(m_activity_masked);

        connect(m_activity, &QListView::clicked, this, &B3DashboardPage::handleActivityClicked);
    }
    layout->addWidget(activityCard, 1);
    layout->addStretch();

    scroll->setWidget(content);

    renderBalances();
    renderWalletState();
}

B3DashboardPage::~B3DashboardPage() = default;

QWidget* B3DashboardPage::makeCard(const QString& title, QLabel** title_label_out)
{
    auto* card = new QFrame(this);
    B3Theme::markCard(card);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(B3Theme::kSpaceMd, B3Theme::kSpaceMd, B3Theme::kSpaceMd, B3Theme::kSpaceMd);
    layout->setSpacing(B3Theme::kSpaceSm);
    auto* title_label = new QLabel(title, card);
    B3Theme::markTextRole(title_label, QStringLiteral("h3"));
    layout->addWidget(title_label);
    if (title_label_out) *title_label_out = title_label;
    return card;
}

QString B3DashboardPage::formatAmount(BitcoinUnit unit, CAmount amount, bool privacy)
{
    return BitcoinUnits::formatWithPrivacy(unit, amount, BitcoinUnits::SeparatorStyle::ALWAYS, privacy);
}

void B3DashboardPage::setClientModel(ClientModel* client_model)
{
    if (m_client_model) {
        disconnect(m_client_model, nullptr, this, nullptr);
    }
    m_client_model = client_model;
    if (!client_model) return;

    connect(client_model, &ClientModel::numBlocksChanged, this, &B3DashboardPage::setNumBlocks);
    connect(client_model, &ClientModel::numConnectionsChanged, this, &B3DashboardPage::setNumConnections);
    connect(client_model, &ClientModel::networkActiveChanged, this, &B3DashboardPage::setNetworkActive);
    connect(client_model->getOptionsModel(), &OptionsModel::fontForMoneyChanged, this, &B3DashboardPage::setMonospacedFont);

    setMonospacedFont(client_model->getOptionsModel()->getFontForMoney());
    m_sync_blocks->setText(tr("Block %1").arg(client_model->getNumBlocks()));
    setNumConnections(client_model->getNumConnections());
    setNetworkActive(client_model->node().getNetworkActive());
}

void B3DashboardPage::setWalletModel(WalletModel* wallet_model)
{
    if (m_wallet_model) {
        disconnect(m_wallet_model, nullptr, this, nullptr);
        if (m_wallet_model->getOptionsModel()) {
            disconnect(m_wallet_model->getOptionsModel(), nullptr, this, nullptr);
        }
    }
    m_activity->setModel(nullptr);
    m_filter.reset();
    m_wallet_model = wallet_model;
    m_have_balances = false;

    if (wallet_model && wallet_model->getOptionsModel()) {
        m_filter = std::make_unique<TransactionFilterProxy>();
        m_filter->setSourceModel(wallet_model->getTransactionTableModel());
        m_filter->setDynamicSortFilter(true);
        m_filter->setSortRole(Qt::EditRole);
        m_filter->setShowInactive(false);
        m_filter->sort(TransactionTableModel::Date, Qt::DescendingOrder);

        m_activity->setModel(m_filter.get());
        m_activity->setModelColumn(TransactionTableModel::ToAddress);

        connect(m_filter.get(), &TransactionFilterProxy::rowsInserted, this, &B3DashboardPage::updateActivityEmptyState);
        connect(m_filter.get(), &TransactionFilterProxy::rowsRemoved, this, &B3DashboardPage::updateActivityEmptyState);
        connect(m_filter.get(), &TransactionFilterProxy::rowsMoved, this, &B3DashboardPage::updateActivityEmptyState);
        connect(m_filter.get(), &TransactionFilterProxy::modelReset, this, &B3DashboardPage::updateActivityEmptyState);

        connect(wallet_model, &WalletModel::balanceChanged, this, &B3DashboardPage::setBalance);
        connect(wallet_model, &WalletModel::encryptionStatusChanged, this, &B3DashboardPage::updateEncryptionStatus);
        connect(wallet_model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &B3DashboardPage::updateDisplayUnit);

        const auto& balances = wallet_model->getCachedBalance();
        if (balances.balance != -1) setBalance(balances);
    }

    updateDisplayUnit();
    renderWalletState();
    updateActivityEmptyState();
}

void B3DashboardPage::showOutOfSyncWarning(bool show)
{
    m_sync_warning->setVisible(show);
}

void B3DashboardPage::setBalance(const interfaces::WalletBalances& balances)
{
    m_balances = balances;
    m_have_balances = true;
    renderBalances();
}

void B3DashboardPage::setPrivacy(bool privacy)
{
    m_privacy = privacy;
    renderBalances();
    updateActivityEmptyState();
}

void B3DashboardPage::setNumBlocks(int count, const QDateTime& block_date, double verification_progress, SyncType /*header*/, SynchronizationState /*sync_state*/)
{
    m_sync_blocks->setText(tr("Block %1").arg(count));
    if (block_date.isValid()) {
        m_sync_time->setText(tr("Last block: %1").arg(GUIUtil::dateTimeStr(block_date)));
    }
    const int permille = static_cast<int>(verification_progress * 1000.0);
    if (permille >= 999) {
        m_sync_progress->hide();
    } else {
        m_sync_progress->setValue(std::max(0, std::min(1000, permille)));
        m_sync_progress->show();
    }
}

void B3DashboardPage::setNumConnections(int count)
{
    m_net_peers->setText(tr("%n active connection(s)", "", count));
}

void B3DashboardPage::setNetworkActive(bool network_active)
{
    m_net_state->setText(network_active ? tr("Network activity enabled") : tr("Network activity disabled"));
}

void B3DashboardPage::updateDisplayUnit()
{
    if (m_wallet_model && m_wallet_model->getOptionsModel()) {
        m_unit = m_wallet_model->getOptionsModel()->getDisplayUnit();
    }
    if (auto* delegate = dynamic_cast<B3ActivityDelegate*>(m_activity->itemDelegate())) {
        delegate->unit = m_unit;
    }
    m_activity->update();
    renderBalances();
}

void B3DashboardPage::updateEncryptionStatus()
{
    renderWalletState();
}

void B3DashboardPage::updateActivityEmptyState()
{
    const bool have_rows = m_filter && m_filter->rowCount() > 0;
    if (m_filter) {
        // Only the most recent rows are shown; the full history lives in
        // the Activity page.
        for (int i = 0; i < m_filter->rowCount(); ++i) {
            m_activity->setRowHidden(i, i >= kActivityRows);
        }
    }
    m_activity->setVisible(!m_privacy && have_rows);
    m_activity_empty->setVisible(!m_privacy && !have_rows);
    m_activity_masked->setVisible(m_privacy);
}

void B3DashboardPage::handleActivityClicked(const QModelIndex& index)
{
    if (m_filter) Q_EMIT transactionClicked(m_filter->mapToSource(index));
}

void B3DashboardPage::setMonospacedFont(const QFont& font)
{
    m_available->setFont(font);
    m_pending->setFont(font);
    m_immature->setFont(font);
    m_total->setFont(font);
}

void B3DashboardPage::renderBalances()
{
    if (!m_have_balances) {
        const QString placeholder = QStringLiteral("—");
        m_available->setText(placeholder);
        m_pending->setText(placeholder);
        m_total->setText(placeholder);
        m_immature->hide();
        m_immature_title->hide();
        return;
    }
    m_available->setText(formatAmount(m_unit, m_balances.balance, m_privacy));
    m_pending->setText(formatAmount(m_unit, m_balances.unconfirmed_balance, m_privacy));
    m_total->setText(formatAmount(m_unit, m_balances.balance + m_balances.unconfirmed_balance + m_balances.immature_balance, m_privacy));
    const bool show_immature = m_balances.immature_balance != 0;
    m_immature->setText(formatAmount(m_unit, m_balances.immature_balance, m_privacy));
    m_immature->setVisible(show_immature);
    m_immature_title->setVisible(show_immature);
}

void B3DashboardPage::renderWalletState()
{
    const bool have_wallet = m_wallet_model != nullptr;
    m_send->setEnabled(have_wallet);
    m_receive->setEnabled(have_wallet);
    m_no_wallet->setVisible(!have_wallet);
    m_wallet_name->setVisible(have_wallet);
    m_security->setVisible(have_wallet);
    if (!have_wallet) return;

    QFontMetrics fm(m_wallet_name->font());
    m_wallet_name->setText(fm.elidedText(m_wallet_model->getDisplayName(), Qt::ElideMiddle, 260));

    switch (m_wallet_model->getEncryptionStatus()) {
    case WalletModel::NoKeys:
        m_security->setText(tr("Watch-only: no private keys"));
        break;
    case WalletModel::Unencrypted:
        m_security->setText(tr("Not encrypted"));
        break;
    case WalletModel::Locked:
        m_security->setText(tr("Encrypted · locked"));
        break;
    case WalletModel::Unlocked:
        m_security->setText(tr("Encrypted · unlocked"));
        break;
    }
}
