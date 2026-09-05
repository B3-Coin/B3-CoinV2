// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/b3dashboardpage.h>

#include <chainparams.h>
#include <consensus/era.h>
#include <qt/b3theme.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/transactionfilterproxy.h>
#include <qt/transactiontablemodel.h>
#include <qt/walletmodel.h>

#include <interfaces/node.h>
#include <validation.h>

#include <QAbstractItemDelegate>
#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
#include <QLocale>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSizePolicy>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kActivityRows{2};
constexpr int kActivityRowHeight{48};

//! Presentation-only hero surface. The underlying card and all of its data
//! remain ordinary Qt widgets; this class only paints the approved subtle
//! honeycomb texture behind them.
class B3BalanceHero final : public QFrame
{
public:
    explicit B3BalanceHero(QWidget* parent) : QFrame{parent} {}

protected:
    void paintEvent(QPaintEvent* event) override
    {
        QFrame::paintEvent(event);

        QPainter painter{this};
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setClipRect(rect().adjusted(1, 1, -1, -1));
        QColor line{B3Theme::kAccent};
        line.setAlpha(14);
        painter.setPen(QPen{line, 1.0});
        painter.setBrush(Qt::NoBrush);

        // 80x69 logical-pixel cells, matching the selected visual reference.
        constexpr qreal radius{40.0};
        constexpr qreal x_step{60.0};
        constexpr qreal y_step{69.2820323028};
        constexpr qreal pi{3.14159265358979323846};
        for (int column = -1; column * x_step < width() + radius; ++column) {
            const qreal center_x{column * x_step};
            const qreal offset_y{(column & 1) ? y_step / 2.0 : 0.0};
            for (int row = -1; row * y_step < height() + radius; ++row) {
                const QPointF center{center_x, row * y_step + offset_y};
                QPolygonF hex;
                for (int point = 0; point < 6; ++point) {
                    const qreal angle{pi * point / 3.0};
                    hex << center + QPointF{std::cos(angle) * radius,
                                             std::sin(angle) * radius};
                }
                painter.drawPolygon(hex);
            }
        }
    }
};

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

        const QRect rect = option.rect.adjusted(B3Theme::kSpaceSm, B3Theme::kSpaceXs,
                                                -B3Theme::kSpaceSm, -B3Theme::kSpaceXs);

        const QString address = index.data(Qt::DisplayRole).toString();
        const QString type = index.sibling(index.row(), TransactionTableModel::Type).data().toString();
        const QDateTime date = index.data(TransactionTableModel::DateRole).toDateTime();
        const qint64 amount = index.data(TransactionTableModel::AmountRole).toLongLong();
        const bool confirmed = index.data(TransactionTableModel::ConfirmedRole).toBool();

        QString amount_text = BitcoinUnits::formatWithUnit(unit, amount, true, BitcoinUnits::SeparatorStyle::ALWAYS);
        if (!confirmed) amount_text = QStringLiteral("[%1]").arg(amount_text);
        const int amount_width = option.fontMetrics.horizontalAdvance(amount_text) + B3Theme::kSpaceMd;

        const QRect icon_rect{rect.left(), rect.center().y() - 13, 26, 26};
        painter->setPen(Qt::NoPen);
        painter->setBrush(B3Theme::kAccentMuted);
        painter->drawEllipse(icon_rect);
        painter->setPen(B3Theme::kAccent);
        painter->drawText(icon_rect, Qt::AlignCenter,
                          amount < 0 ? QStringLiteral("↑") : QStringLiteral("↓"));

        painter->setPen(amount < 0 ? B3Theme::kNegative : (confirmed ? B3Theme::kPositive : B3Theme::kTextMuted));
        painter->drawText(rect, Qt::AlignRight | Qt::AlignVCenter, amount_text);

        const int copy_left{icon_rect.right() + B3Theme::kSpaceMd};
        const int copy_width{std::max(0, rect.right() - amount_width - copy_left)};
        const QRect copy_rect{copy_left, rect.top(), copy_width, rect.height()};
        painter->setPen(B3Theme::kTextPrimary);
        const QString primary{option.fontMetrics.elidedText(type.isEmpty() ? address : type,
                                                            Qt::ElideRight, copy_rect.width() / 2)};
        painter->drawText(copy_rect, Qt::AlignLeft | Qt::AlignVCenter, primary);

        const int primary_width{option.fontMetrics.horizontalAdvance(primary)};
        painter->setPen(B3Theme::kTextSecondary);
        const QRect secondary_rect{copy_rect.left() + primary_width + B3Theme::kSpaceSm,
                                   copy_rect.top(),
                                   std::max(0, copy_rect.width() - primary_width - B3Theme::kSpaceSm),
                                   copy_rect.height()};
        painter->drawText(secondary_rect, Qt::AlignLeft | Qt::AlignVCenter,
                          option.fontMetrics.elidedText(GUIUtil::dateTimeStr(date), Qt::ElideRight,
                                                        secondary_rect.width()));

        QColor divider{B3Theme::kBorder};
        divider.setAlpha(180);
        painter->setPen(divider);
        painter->drawLine(rect.left(), option.rect.bottom(), rect.right(), option.rect.bottom());

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
    content->setObjectName(QStringLiteral("dashboardViewport"));
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(B3Theme::kSpaceLg, B3Theme::kSpaceLg,
                               B3Theme::kSpaceLg, B3Theme::kSpaceXl);
    layout->setSpacing(B3Theme::kSpaceLg);

    m_card_grid = new QGridLayout();
    m_card_grid->setContentsMargins(0, 0, 0, 0);
    m_card_grid->setHorizontalSpacing(B3Theme::kSpaceMd);
    m_card_grid->setVerticalSpacing(B3Theme::kSpaceMd);

    QLabel* balanceTitle{nullptr};
    m_balance_card = makeCard(tr("AVAILABLE BALANCE"), &balanceTitle, /*hero=*/true);
    m_balance_card->setObjectName(QStringLiteral("dashboardBalanceCard"));
    m_balance_card->setProperty("b3surface", QStringLiteral("hero"));
    m_balance_card->setMinimumHeight(182);
    B3Theme::markTextRole(balanceTitle, QStringLiteral("heroEyebrow"));
    {
        auto* cardLayout = static_cast<QVBoxLayout*>(m_balance_card->layout());
        cardLayout->setContentsMargins(28, 24, 28, 24);
        m_available = makeValueLabel(m_balance_card);
        m_available->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        m_available->setObjectName(QStringLiteral("dashboardAvailableBalance"));
        B3Theme::markTextRole(m_available, QStringLiteral("balance"));
        auto* balanceRow = new QHBoxLayout();
        balanceRow->setContentsMargins(0, 0, 0, 0);
        balanceRow->setSpacing(B3Theme::kSpaceSm);
        balanceRow->addWidget(m_available);
        auto* balanceUnit = new QLabel(tr("B3"), m_balance_card);
        B3Theme::markTextRole(balanceUnit, QStringLiteral("balanceUnit"));
        balanceRow->addWidget(balanceUnit, 0, Qt::AlignBottom);
        balanceRow->addStretch();
        cardLayout->addLayout(balanceRow);

        auto* availableNote = new QLabel(tr("Confirmed funds ready to spend"), m_balance_card);
        B3Theme::markTextRole(availableNote, QStringLiteral("muted"));
        cardLayout->addWidget(availableNote);

        auto* buttons = new QHBoxLayout();
        buttons->setSpacing(B3Theme::kSpaceSm);
        m_send = new QPushButton(tr("Send B3"), m_balance_card);
        m_receive = new QPushButton(tr("Receive"), m_balance_card);
        m_send->setObjectName(QStringLiteral("dashboardSend"));
        m_receive->setObjectName(QStringLiteral("dashboardReceive"));
        m_send->setProperty("b3variant", QStringLiteral("primary"));
        buttons->addWidget(m_send);
        buttons->addWidget(m_receive);
        buttons->addStretch();
        cardLayout->addLayout(buttons);
        cardLayout->addStretch();

        connect(m_send, &QPushButton::clicked, this, &B3DashboardPage::sendRequested);
        connect(m_receive, &QPushButton::clicked, this, &B3DashboardPage::receiveRequested);
    }

    m_wallet_card = makeCard(tr("Wallet"));
    m_wallet_card->setObjectName(QStringLiteral("dashboardWalletCard"));
    m_wallet_card->setMinimumHeight(154);
    {
        auto* cardLayout = static_cast<QVBoxLayout*>(m_wallet_card->layout());
        m_no_wallet = new QLabel(
            tr("No wallet is loaded. Open or create a wallet to send and receive B3."),
            m_wallet_card);
        B3Theme::markTextRole(m_no_wallet, QStringLiteral("secondary"));
        m_no_wallet->setWordWrap(true);
        cardLayout->addWidget(m_no_wallet);

        auto addMetric = [&](const QString& title, QLabel** value_out, QLabel** title_out = nullptr) {
            auto* row = new QHBoxLayout();
            auto* t = new QLabel(title, m_wallet_card);
            B3Theme::markTextRole(t, QStringLiteral("secondary"));
            auto* v = makeValueLabel(m_wallet_card);
            B3Theme::markTextRole(v, QStringLiteral("title"));
            row->addWidget(t);
            row->addStretch();
            row->addWidget(v);
            cardLayout->addLayout(row);
            if (title_out) *title_out = t;
            *value_out = v;
        };
        addMetric(tr("Security"), &m_security);
        addMetric(tr("Pending"), &m_pending);
        addMetric(tr("Immature"), &m_immature, &m_immature_title);
        cardLayout->addStretch();
    }

    m_staking_card = makeCard(tr("Staking"));
    m_staking_card->setObjectName(QStringLiteral("dashboardStakingCard"));
    m_staking_card->setProperty("b3surface", QStringLiteral("quiet"));
    m_staking_card->hide();
    {
        m_staking_note = new QLabel(
            tr("Staking information is unavailable in this build. No estimated rewards or network weight are invented."),
            m_staking_card);
        m_staking_note->setWordWrap(true);
        B3Theme::markTextRole(m_staking_note, QStringLiteral("secondary"));
        static_cast<QVBoxLayout*>(m_staking_card->layout())->addWidget(m_staking_note);
        static_cast<QVBoxLayout*>(m_staking_card->layout())->addStretch();
    }

    QLabel* eraTitle{nullptr};
    m_sync_card = makeCard(tr("NETWORK ERA"), &eraTitle);
    m_sync_card->setObjectName(QStringLiteral("dashboardSyncCard"));
    m_sync_card->setMinimumHeight(152);
    B3Theme::markTextRole(eraTitle, QStringLiteral("eyebrowMuted"));
    {
        auto* cardLayout = static_cast<QVBoxLayout*>(m_sync_card->layout());
        m_sync_blocks = new QLabel(tr("Waiting for chain data…"), m_sync_card);
        m_sync_blocks->setObjectName(QStringLiteral("dashboardEraName"));
        B3Theme::markTextRole(m_sync_blocks, QStringLiteral("h1"));
        cardLayout->addWidget(m_sync_blocks);
        m_sync_time = new QLabel(tr("The connected chain determines which protocol era is displayed."), m_sync_card);
        B3Theme::markTextRole(m_sync_time, QStringLiteral("secondary"));
        m_sync_time->setWordWrap(true);
        cardLayout->addWidget(m_sync_time);
        m_sync_progress_caption = new QLabel(tr("Chain synchronization"), m_sync_card);
        B3Theme::markTextRole(m_sync_progress_caption, QStringLiteral("muted"));
        m_sync_progress_caption->hide();
        cardLayout->addWidget(m_sync_progress_caption);
        m_sync_progress = new QProgressBar(m_sync_card);
        m_sync_progress->setRange(0, 1000);
        m_sync_progress->setTextVisible(false);
        m_sync_progress->setFixedHeight(6);
        m_sync_progress->setAccessibleName(tr("Chain synchronization progress"));
        m_sync_progress->setToolTip(tr("Chain synchronization progress"));
        m_sync_progress->hide();
        cardLayout->addWidget(m_sync_progress);

        auto* heightRow = new QHBoxLayout();
        m_era_height = new QLabel(QStringLiteral("—"), m_sync_card);
        m_era_height->setObjectName(QStringLiteral("dashboardEraHeight"));
        B3Theme::markTextRole(m_era_height, QStringLiteral("secondary"));
        auto* heightCaption = new QLabel(tr("Current chain height"), m_sync_card);
        B3Theme::markTextRole(heightCaption, QStringLiteral("secondary"));
        heightRow->addWidget(m_era_height);
        heightRow->addStretch();
        heightRow->addWidget(heightCaption);
        cardLayout->addLayout(heightRow);

        m_sync_warning = new QLabel(tr("Displayed information may be out of date."), m_sync_card);
        m_sync_warning->setWordWrap(true);
        B3Theme::markTextRole(m_sync_warning, QStringLiteral("status"));
        m_sync_warning->hide();
        cardLayout->addWidget(m_sync_warning);
        cardLayout->addStretch();
    }

    m_network_card = makeCard(tr("Node"));
    m_network_card->setObjectName(QStringLiteral("dashboardNetworkCard"));
    m_network_card->setMinimumHeight(152);
    {
        auto* cardLayout = static_cast<QVBoxLayout*>(m_network_card->layout());
        auto addMetric = [&](const QString& title, QLabel** value_out, const char* object_name) {
            auto* row = new QHBoxLayout();
            auto* label = new QLabel(title, m_network_card);
            B3Theme::markTextRole(label, QStringLiteral("secondary"));
            auto* value = new QLabel(QStringLiteral("—"), m_network_card);
            value->setObjectName(QLatin1String(object_name));
            B3Theme::markTextRole(value, QStringLiteral("title"));
            value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            row->addWidget(label);
            row->addStretch();
            row->addWidget(value);
            cardLayout->addLayout(row);
            *value_out = value;
        };
        addMetric(tr("Connections"), &m_net_peers, "dashboardConnections");
        addMetric(tr("Network"), &m_net_network, "dashboardNetwork");
        addMetric(tr("State"), &m_net_state, "dashboardNetworkState");
        cardLayout->addStretch();
    }

    // Recent activity.
    m_activity_card = makeCard(tr("Recent activity"));
    m_activity_card->setObjectName(QStringLiteral("dashboardActivityCard"));
    {
        auto* cardLayout = static_cast<QVBoxLayout*>(m_activity_card->layout());
        m_activity = new QListView(m_activity_card);
        m_activity->setObjectName(QStringLiteral("dashboardActivity"));
        m_activity->setItemDelegate(new B3ActivityDelegate(m_activity));
        m_activity->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_activity->setFrameShape(QFrame::NoFrame);
        m_activity->setAttribute(Qt::WA_MacShowFocusRect, false);
        m_activity->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_activity->setMinimumHeight(kActivityRows * kActivityRowHeight);
        m_activity->hide();
        cardLayout->addWidget(m_activity);

        m_activity_empty = new QLabel(tr("No transactions yet."), m_activity_card);
        B3Theme::markTextRole(m_activity_empty, QStringLiteral("secondary"));
        m_activity_empty->setAlignment(Qt::AlignCenter);
        m_activity_empty->setMinimumHeight(96);
        cardLayout->addWidget(m_activity_empty);

        m_activity_masked = new QLabel(tr("Values are masked. Uncheck Settings → Mask values to show activity."), m_activity_card);
        B3Theme::markTextRole(m_activity_masked, QStringLiteral("secondary"));
        m_activity_masked->setWordWrap(true);
        m_activity_masked->setAlignment(Qt::AlignCenter);
        m_activity_masked->setMinimumHeight(96);
        m_activity_masked->hide();
        cardLayout->addWidget(m_activity_masked);

        connect(m_activity, &QListView::clicked, this, &B3DashboardPage::handleActivityClicked);
    }
    // The approved direction uses calm, content-height cards. Let the page's
    // trailing stretch absorb tall windows instead of inflating every card.
    layout->addLayout(m_card_grid);
    layout->addStretch();

    scroll->setWidget(content);
    reflowCards(width());

    renderBalances();
    renderWalletState();
    renderNodeState();
}

B3DashboardPage::~B3DashboardPage() = default;

QWidget* B3DashboardPage::makeCard(const QString& title, QLabel** title_label_out, bool hero)
{
    QFrame* card = hero ? static_cast<QFrame*>(new B3BalanceHero(this)) : new QFrame(this);
    B3Theme::markCard(card);
    card->setProperty("b3surface", QStringLiteral("panel"));
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(18, 17, 18, 17);
    layout->setSpacing(B3Theme::kSpaceSm);
    auto* title_label = new QLabel(title, card);
    B3Theme::markTextRole(title_label, QStringLiteral("h3"));
    layout->addWidget(title_label);
    if (title_label_out) *title_label_out = title_label;
    return card;
}

void B3DashboardPage::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    reflowCards(event->size().width());
}

void B3DashboardPage::reflowCards(int width)
{
    if (!m_card_grid || !m_activity_card) return;

    // The approved 736px composition is intentionally single-column. Keep
    // that calm stack through ordinary wallet-window sizes and fan out only
    // when the workspace is genuinely wide.
    const int columns = width < 980 ? 1 : (width < 1200 ? 2 : 3);
    if (columns == m_layout_columns) return;
    m_layout_columns = columns;

    for (QWidget* card : {m_balance_card, m_wallet_card, m_staking_card,
                          m_sync_card, m_network_card, m_activity_card}) {
        m_card_grid->removeWidget(card);
    }
    m_staking_card->hide();
    for (int column = 0; column < 3; ++column) {
        m_card_grid->setColumnStretch(column, 0);
    }

    if (columns == 1) {
        m_card_grid->addWidget(m_balance_card, 0, 0);
        m_card_grid->addWidget(m_sync_card, 1, 0);
        m_card_grid->addWidget(m_wallet_card, 2, 0);
        m_card_grid->addWidget(m_network_card, 3, 0);
        m_card_grid->addWidget(m_activity_card, 4, 0);
    } else if (columns == 2) {
        m_card_grid->addWidget(m_balance_card, 0, 0, 1, 2);
        m_card_grid->addWidget(m_sync_card, 1, 0);
        m_card_grid->addWidget(m_wallet_card, 1, 1);
        m_card_grid->addWidget(m_network_card, 2, 0, 1, 2);
        m_card_grid->addWidget(m_activity_card, 3, 0, 1, 2);
    } else {
        m_card_grid->addWidget(m_balance_card, 0, 0, 1, 3);
        m_card_grid->addWidget(m_sync_card, 1, 0);
        m_card_grid->addWidget(m_wallet_card, 1, 1);
        m_card_grid->addWidget(m_network_card, 1, 2);
        m_card_grid->addWidget(m_activity_card, 2, 0, 1, 3);
    }
    for (int column = 0; column < columns; ++column) {
        m_card_grid->setColumnStretch(column, 1);
    }
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
    if (!client_model) {
        m_chain_height = -1;
        m_chain_synced = false;
        renderNetworkEra(m_chain_height);
        renderNodeState();
        return;
    }

    connect(client_model, &ClientModel::numBlocksChanged, this, &B3DashboardPage::setNumBlocks);
    connect(client_model, &ClientModel::numConnectionsChanged, this, &B3DashboardPage::setNumConnections);
    connect(client_model, &ClientModel::networkActiveChanged, this, &B3DashboardPage::setNetworkActive);
    connect(client_model->getOptionsModel(), &OptionsModel::fontForMoneyChanged, this, &B3DashboardPage::setMonospacedFont);

    setMonospacedFont(client_model->getOptionsModel()->getFontForMoney());
    m_chain_height = client_model->getNumBlocks();
    m_chain_synced = !client_model->node().isInitialBlockDownload();
    renderNetworkEra(m_chain_height);
    m_sync_progress->setValue(std::clamp(
        static_cast<int>(client_model->node().getVerificationProgress() * 1000.0), 0, 1000));
    m_sync_progress_caption->show();
    m_sync_progress->show();
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

void B3DashboardPage::setNumBlocks(int count, const QDateTime& block_date, double verification_progress,
                                   SyncType header, SynchronizationState sync_state)
{
    Q_UNUSED(block_date);
    // Header notifications can be far ahead of the connected chain. Era and
    // current-height presentation must only follow connected BLOCK_SYNC tips.
    if (header != SyncType::BLOCK_SYNC) return;

    m_chain_height = count;
    m_chain_synced = sync_state == SynchronizationState::POST_INIT;
    renderNetworkEra(count);
    renderNodeState();

    const int permille = static_cast<int>(verification_progress * 1000.0);
    m_sync_progress->setValue(std::max(0, std::min(1000, permille)));
    m_sync_progress_caption->show();
    m_sync_progress->show();
}

void B3DashboardPage::setNumConnections(int count)
{
    m_net_peers->setText(tr("%n peer(s)", "", count));
}

void B3DashboardPage::setNetworkActive(bool network_active)
{
    m_network_active = network_active;
    renderNodeState();
}

void B3DashboardPage::renderNetworkEra(int height)
{
    if (height < 0) {
        m_sync_blocks->setText(tr("Waiting for chain data…"));
        m_sync_time->setText(tr("The connected chain determines which protocol era is displayed."));
        m_era_height->setText(QStringLiteral("—"));
        return;
    }

    const Consensus::Params& params{Params().GetConsensus()};
    QString era_name;
    QString detail;

    if (!params.legacy_b3coin) {
        era_name = tr("Test network");
        detail = tr("This chain does not use the live legacy B3 transition boundary.");
    } else if (!Consensus::LegacyFinalHeight(params)) {
        era_name = tr("Legacy network");
        detail = tr("Modern protocol components are installed but inactive. The wallet continues normal legacy operation.");
    } else if (Consensus::LegacyBoundaryHeightOnly(params)) {
        era_name = tr("Legacy network");
        detail = tr("The final legacy height is configured, but anchor X is not pinned. The transition remains paused.");
    } else {
        // The era of the NEXT block: that is the phase the node operates in
        // (its peer selection, banner and block rules), and what the Stake
        // page reports. A node holding the sealed boundary block itself is
        // already past the legacy network and must be shown as such.
        switch (Consensus::GetConsensusPhase(height + 1, params)) {
        case Consensus::ConsensusPhase::LEGACY_POS:
            era_name = tr("Legacy network");
            detail = tr("Legacy operation continues through the sealed boundary at block %1.")
                         .arg(*Consensus::LegacyFinalHeight(params));
            break;
        case Consensus::ConsensusPhase::TRANSITION_POW:
            era_name = tr("Transition corridor");
            detail = tr("The temporary proof-of-work corridor is active on the connected chain.");
            break;
        case Consensus::ConsensusPhase::MODERN_POS:
            if (Consensus::ModernObjectRulesActive(params)) {
                era_name = tr("Modern PoS");
                detail = tr("Modern proof of stake is active on the connected chain.");
            } else {
                era_name = tr("Modern PoS unavailable");
                detail = tr("The modern proof-of-stake rules are not completely configured; the node fails closed.");
            }
            break;
        }
    }

    m_sync_blocks->setText(era_name);
    m_sync_time->setText(detail);
    m_era_height->setText(QLocale{}.toString(height));
}

void B3DashboardPage::renderNodeState()
{
    QString network{QString::fromStdString(Params().GetChainTypeString())};
    if (network == QLatin1String("main")) network = tr("Mainnet");
    else if (network == QLatin1String("test")) network = tr("Testnet");
    else if (network == QLatin1String("testnet4")) network = tr("Testnet4");
    else if (network == QLatin1String("regtest")) network = tr("Regtest");
    else if (network == QLatin1String("signet")) network = tr("Signet");
    m_net_network->setText(network);

    const bool synchronized{m_network_active && m_chain_height >= 0 && m_chain_synced};
    m_net_state->setText(!m_network_active ? tr("Network disabled")
                         : synchronized ? tr("Synchronized")
                         : m_chain_height >= 0 ? tr("Synchronizing")
                                               : tr("Waiting for chain"));
    m_net_state->setProperty("b3state", synchronized ? QStringLiteral("synced")
                                                       : QStringLiteral("waiting"));
    m_net_state->style()->unpolish(m_net_state);
    m_net_state->style()->polish(m_net_state);
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
}

void B3DashboardPage::renderBalances()
{
    if (!m_have_balances) {
        const QString placeholder = QStringLiteral("—");
        m_available->setText(placeholder);
        m_pending->setText(placeholder);
        m_immature->setText(placeholder);
        return;
    }
    m_available->setText(formatAmount(m_unit, m_balances.balance, m_privacy));
    m_pending->setText(formatAmount(m_unit, m_balances.unconfirmed_balance, m_privacy));
    m_immature->setText(formatAmount(m_unit, m_balances.immature_balance, m_privacy));
}

void B3DashboardPage::renderWalletState()
{
    const bool have_wallet = m_wallet_model != nullptr;
    m_send->setEnabled(have_wallet);
    m_receive->setEnabled(have_wallet);
    m_no_wallet->setVisible(!have_wallet);
    if (!have_wallet) {
        m_security->setText(QStringLiteral("—"));
        return;
    }

    switch (m_wallet_model->getEncryptionStatus()) {
    case WalletModel::NoKeys:
        m_security->setText(tr("Watch-only"));
        break;
    case WalletModel::Unencrypted:
        m_security->setText(tr("Not encrypted"));
        break;
    case WalletModel::Locked:
        m_security->setText(tr("Locked"));
        break;
    case WalletModel::Unlocked:
        m_security->setText(tr("Unlocked"));
        break;
    }
}
