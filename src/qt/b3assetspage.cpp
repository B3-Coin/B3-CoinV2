// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/b3assetspage.h>

#include <qt/b3theme.h>
#include <qt/walletmodel.h>

#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QVBoxLayout>

namespace {
QLabel* makeDetailValue(QWidget* parent)
{
    auto* label = new QLabel(QStringLiteral("—"), parent);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return label;
}
} // namespace

B3AssetsPage::B3AssetsPage(QWidget* parent)
    : QWidget{parent}
{
    m_model = new B3AssetTableModel(this);
    m_proxy = new QSortFilterProxyModel(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->setFilterKeyColumn(-1); // search across name and ticker

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);

    auto* content = new QWidget(scroll);
    content->setObjectName(QStringLiteral("assetsViewport"));
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(B3Theme::kSpaceLg, B3Theme::kSpaceLg,
                               B3Theme::kSpaceLg, B3Theme::kSpaceXl);
    layout->setSpacing(B3Theme::kSpaceLg);

    auto* eyebrow = new QLabel(tr("PORTFOLIO"), content);
    B3Theme::markTextRole(eyebrow, QStringLiteral("eyebrow"));
    layout->addWidget(eyebrow);
    auto* heading = new QLabel(tr("Assets"), content);
    B3Theme::markTextRole(heading, QStringLiteral("h1"));
    layout->addWidget(heading);
    auto* introduction = new QLabel(
        tr("Native B3 is available now. Additional assets and FlowMesh balances appear only when a verified backend provides them."),
        content);
    introduction->setWordWrap(true);
    B3Theme::markTextRole(introduction, QStringLiteral("secondary"));
    layout->addWidget(introduction);

    m_columns = new QGridLayout();
    m_columns->setContentsMargins(0, 0, 0, 0);
    m_columns->setHorizontalSpacing(B3Theme::kSpaceMd);
    m_columns->setVerticalSpacing(B3Theme::kSpaceMd);

    // Left: searchable asset list.
    m_list_card = new QFrame(content);
    m_list_card->setObjectName(QStringLiteral("assetsListCard"));
    B3Theme::markCard(m_list_card);
    m_list_card->setProperty("b3surface", QStringLiteral("panel"));
    m_list_card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    {
        auto* listLayout = new QVBoxLayout(m_list_card);
        listLayout->setContentsMargins(B3Theme::kSpaceLg, B3Theme::kSpaceLg,
                                       B3Theme::kSpaceLg, B3Theme::kSpaceLg);
        listLayout->setSpacing(B3Theme::kSpaceSm);

        auto* listTitle = new QLabel(tr("Your assets"), m_list_card);
        B3Theme::markTextRole(listTitle, QStringLiteral("h3"));
        listLayout->addWidget(listTitle);

        m_search = new QLineEdit(m_list_card);
        m_search->setObjectName(QStringLiteral("assetSearch"));
        m_search->setPlaceholderText(tr("Search by name or ticker"));
        m_search->setClearButtonEnabled(true);
        listLayout->addWidget(m_search);
        connect(m_search, &QLineEdit::textChanged, m_proxy,
                qOverload<const QString&>(&QSortFilterProxyModel::setFilterFixedString));

        m_list = new QTableView(m_list_card);
        m_list->setObjectName(QStringLiteral("assetList"));
        m_list->setModel(m_proxy);
        m_list->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_list->setSelectionMode(QAbstractItemView::SingleSelection);
        m_list->setShowGrid(false);
        m_list->verticalHeader()->setVisible(false);
        m_list->horizontalHeader()->setStretchLastSection(true);
        m_list->horizontalHeader()->setSectionResizeMode(B3AssetTableModel::Name, QHeaderView::Stretch);
        m_list->setFrameShape(QFrame::NoFrame);
        m_list->setAlternatingRowColors(true);
        m_list->setMinimumHeight(300);
        m_list->verticalHeader()->setDefaultSectionSize(44);
        listLayout->addWidget(m_list, 1);

        m_empty = new QLabel(tr("No wallet is loaded."), m_list_card);
        B3Theme::markTextRole(m_empty, QStringLiteral("secondary"));
        m_empty->setWordWrap(true);
        m_empty->setAlignment(Qt::AlignCenter);
        m_empty->setMinimumHeight(72);
        listLayout->addWidget(m_empty);

        connect(m_list->selectionModel(), &QItemSelectionModel::currentRowChanged,
                this, &B3AssetsPage::updateDetails);
        connect(m_model, &QAbstractItemModel::modelReset, this, &B3AssetsPage::updateDetails);
    }

    // Right: selected-asset details and actions.
    m_detail_card = new QFrame(content);
    m_detail_card->setObjectName(QStringLiteral("assetsDetailCard"));
    B3Theme::markCard(m_detail_card);
    m_detail_card->setProperty("b3surface", QStringLiteral("hero"));
    m_detail_card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    {
        auto* detailLayout = new QVBoxLayout(m_detail_card);
        detailLayout->setContentsMargins(B3Theme::kSpaceLg, B3Theme::kSpaceLg,
                                         B3Theme::kSpaceLg, B3Theme::kSpaceLg);
        detailLayout->setSpacing(B3Theme::kSpaceSm);

        auto* detailEyebrow = new QLabel(tr("SELECTED ASSET"), m_detail_card);
        B3Theme::markTextRole(detailEyebrow, QStringLiteral("eyebrow"));
        detailLayout->addWidget(detailEyebrow);

        m_detail_name = new QLabel(m_detail_card);
        B3Theme::markTextRole(m_detail_name, QStringLiteral("h2"));
        m_detail_name->setTextInteractionFlags(Qt::TextSelectableByMouse);
        detailLayout->addWidget(m_detail_name);

        m_detail_status = new QLabel(m_detail_card);
        B3Theme::markTextRole(m_detail_status, QStringLiteral("status"));
        detailLayout->addWidget(m_detail_status);
        detailLayout->addSpacing(B3Theme::kSpaceSm);

        auto addBalanceRow = [&](const QString& title, QLabel** value_out) {
            auto* row = new QHBoxLayout();
            auto* t = new QLabel(title, m_detail_card);
            B3Theme::markTextRole(t, QStringLiteral("secondary"));
            row->addWidget(t);
            row->addStretch();
            *value_out = makeDetailValue(m_detail_card);
            B3Theme::markTextRole(*value_out, QStringLiteral("title"));
            row->addWidget(*value_out);
            detailLayout->addLayout(row);
        };
        addBalanceRow(tr("Confirmed"), &m_detail_confirmed);
        addBalanceRow(tr("Pending"), &m_detail_pending);
        addBalanceRow(tr("Available"), &m_detail_available);
        addBalanceRow(tr("Reserved"), &m_detail_reserved);
        addBalanceRow(tr("In FlowMesh"), &m_detail_flowmesh);

        detailLayout->addSpacing(B3Theme::kSpaceSm);

        auto* actionRow = new QHBoxLayout();
        m_send = new QPushButton(tr("Send"), m_detail_card);
        m_send->setObjectName(QStringLiteral("assetSend"));
        m_send->setProperty("b3variant", QStringLiteral("primary"));
        m_receive = new QPushButton(tr("Receive"), m_detail_card);
        m_receive->setObjectName(QStringLiteral("assetReceive"));
        actionRow->addWidget(m_send);
        actionRow->addWidget(m_receive);
        actionRow->addStretch();
        detailLayout->addLayout(actionRow);

        auto* meshRow = new QHBoxLayout();
        m_deposit = new QPushButton(tr("Deposit to FlowMesh"), m_detail_card);
        m_deposit->setObjectName(QStringLiteral("assetDeposit"));
        m_withdraw = new QPushButton(tr("Withdraw from FlowMesh"), m_detail_card);
        m_withdraw->setObjectName(QStringLiteral("assetWithdraw"));
        meshRow->addWidget(m_deposit);
        meshRow->addWidget(m_withdraw);
        meshRow->addStretch();
        detailLayout->addLayout(meshRow);

        m_backend_note = new QLabel(m_detail_card);
        B3Theme::markTextRole(m_backend_note, QStringLiteral("status"));
        m_backend_note->setWordWrap(true);
        detailLayout->addWidget(m_backend_note);

        m_activity_note = new QLabel(m_detail_card);
        B3Theme::markTextRole(m_activity_note, QStringLiteral("muted"));
        m_activity_note->setWordWrap(true);
        detailLayout->addWidget(m_activity_note);

        detailLayout->addStretch();

        connect(m_send, &QPushButton::clicked, this, &B3AssetsPage::sendRequested);
        connect(m_receive, &QPushButton::clicked, this, &B3AssetsPage::receiveRequested);
        // Deposit/withdraw stay disconnected as well as disabled: there is
        // no backend to submit to.
    }
    layout->addLayout(m_columns, 1);
    scroll->setWidget(content);
    reflowCards(width());

    updateDetails();
}

void B3AssetsPage::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    reflowCards(event->size().width());
}

void B3AssetsPage::reflowCards(int width)
{
    if (!m_columns || !m_detail_card) return;
    const int columns = width < 880 ? 1 : 2;
    if (columns == m_layout_columns) return;
    m_layout_columns = columns;

    m_columns->removeWidget(m_list_card);
    m_columns->removeWidget(m_detail_card);
    m_columns->setColumnStretch(0, 0);
    m_columns->setColumnStretch(1, 0);
    if (columns == 1) {
        m_columns->addWidget(m_list_card, 0, 0);
        m_columns->addWidget(m_detail_card, 1, 0);
        m_columns->setColumnStretch(0, 1);
    } else {
        m_columns->addWidget(m_list_card, 0, 0);
        m_columns->addWidget(m_detail_card, 0, 1);
        m_columns->setColumnStretch(0, 2);
        m_columns->setColumnStretch(1, 3);
    }
}

void B3AssetsPage::setWalletModel(WalletModel* wallet_model)
{
    m_have_wallet = wallet_model != nullptr;
    B3AssetSource* old = m_owned_source;
    if (m_have_wallet) {
        m_owned_source = new B3NativeAssetSource(wallet_model, this);
        m_model->setSource(m_owned_source);
    } else {
        m_owned_source = nullptr;
        m_model->setSource(nullptr);
    }
    delete old;
    updateDetails();
}

void B3AssetsPage::setSource(B3AssetSource* source)
{
    B3AssetSource* old = m_owned_source;
    m_owned_source = nullptr;
    m_have_wallet = source != nullptr;
    m_model->setSource(source);
    delete old;
    updateDetails();
}

void B3AssetsPage::updateDetails()
{
    const int rows = m_proxy->rowCount();
    m_empty->setVisible(rows == 0);
    m_empty->setText(m_have_wallet ? tr("No assets to show.") : tr("No wallet is loaded."));

    // Ensure something is selected whenever rows exist.
    if (rows > 0 && !m_list->currentIndex().isValid()) {
        m_list->setCurrentIndex(m_proxy->index(0, 0));
    }

    B3AssetRecord record;
    bool have_selection = false;
    const QModelIndex current = m_list->currentIndex();
    if (current.isValid()) {
        record = m_model->recordAt(m_proxy->mapToSource(current).row());
        have_selection = !record.asset_id.isEmpty();
    }

    if (!have_selection) {
        m_detail_name->setText(tr("No asset selected"));
        m_detail_status->clear();
        for (QLabel* value : {m_detail_confirmed, m_detail_pending, m_detail_available,
                              m_detail_reserved, m_detail_flowmesh}) {
            value->setText(QStringLiteral("—"));
        }
        m_send->setEnabled(false);
        m_receive->setEnabled(false);
        m_deposit->setEnabled(false);
        m_withdraw->setEnabled(false);
        m_backend_note->setVisible(false);
        m_activity_note->setVisible(false);
        return;
    }

    const QString name = record.metadata_known ? record.display_name : tr("Unknown asset");
    QFontMetrics fm(m_detail_name->font());
    m_detail_name->setText(fm.elidedText(name + QStringLiteral(" (") + record.ticker + QStringLiteral(")"),
                                         Qt::ElideMiddle, 320));
    switch (record.status) {
    case B3AssetRecord::Status::Native:
        m_detail_status->setText(tr("Native coin · real wallet balance"));
        break;
    case B3AssetRecord::Status::Active:
        m_detail_status->setText(tr("Coloured asset"));
        break;
    case B3AssetRecord::Status::Unavailable:
        m_detail_status->setText(tr("Backend unavailable"));
        break;
    }

    const auto amount = [&](CAmount value) { return B3AssetTableModel::formatAmount(value, record.decimals); };
    m_detail_confirmed->setText(amount(record.confirmed));
    m_detail_pending->setText(amount(record.pending));
    m_detail_available->setText(amount(record.available));
    m_detail_reserved->setText(record.reserved_available ? amount(record.reserved) : tr("Not available"));
    m_detail_flowmesh->setText(record.flowmesh_available ? amount(record.flowmesh) : tr("Not available"));

    // Only native B3 with a wallet attached can act; everything else is
    // visibly disabled with the reason stated.
    const bool native = record.status == B3AssetRecord::Status::Native;
    m_send->setEnabled(native && m_have_wallet);
    m_receive->setEnabled(native && m_have_wallet);

    // The model can expose FlowMesh balances, but this page has no approved
    // deposit/withdraw submission path yet. Never turn disconnected buttons
    // into controls that merely look live.
    m_deposit->setEnabled(false);
    m_withdraw->setEnabled(false);
    m_backend_note->setVisible(true);
    m_backend_note->setText(tr("FlowMesh deposits and withdrawals are not active in this "
                               "wallet build."));

    m_activity_note->setVisible(true);
    m_activity_note->setText(native
        ? tr("Native B3 transactions are listed on the Activity page.")
        : tr("Asset activity requires a coloured-asset backend, which is not "
             "available in this build."));
}
