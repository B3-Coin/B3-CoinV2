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
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
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

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(B3Theme::kSpaceLg, B3Theme::kSpaceLg, B3Theme::kSpaceLg, B3Theme::kSpaceLg);
    layout->setSpacing(B3Theme::kSpaceMd);

    auto* heading = new QLabel(tr("Assets"), this);
    B3Theme::markTextRole(heading, QStringLiteral("h1"));
    layout->addWidget(heading);

    auto* columns = new QHBoxLayout();
    columns->setSpacing(B3Theme::kSpaceMd);

    // Left: searchable asset list.
    auto* listCard = new QFrame(this);
    B3Theme::markCard(listCard);
    {
        auto* listLayout = new QVBoxLayout(listCard);
        listLayout->setContentsMargins(B3Theme::kSpaceMd, B3Theme::kSpaceMd, B3Theme::kSpaceMd, B3Theme::kSpaceMd);
        listLayout->setSpacing(B3Theme::kSpaceSm);

        m_search = new QLineEdit(listCard);
        m_search->setObjectName(QStringLiteral("assetSearch"));
        m_search->setPlaceholderText(tr("Search assets"));
        m_search->setClearButtonEnabled(true);
        listLayout->addWidget(m_search);
        connect(m_search, &QLineEdit::textChanged, m_proxy,
                qOverload<const QString&>(&QSortFilterProxyModel::setFilterFixedString));

        m_list = new QTableView(listCard);
        m_list->setObjectName(QStringLiteral("assetList"));
        m_list->setModel(m_proxy);
        m_list->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_list->setSelectionMode(QAbstractItemView::SingleSelection);
        m_list->setShowGrid(false);
        m_list->verticalHeader()->setVisible(false);
        m_list->horizontalHeader()->setStretchLastSection(true);
        m_list->horizontalHeader()->setSectionResizeMode(B3AssetTableModel::Name, QHeaderView::Stretch);
        m_list->setFrameShape(QFrame::NoFrame);
        listLayout->addWidget(m_list, 1);

        m_empty = new QLabel(tr("No wallet is loaded."), listCard);
        B3Theme::markTextRole(m_empty, QStringLiteral("secondary"));
        m_empty->setWordWrap(true);
        listLayout->addWidget(m_empty);

        connect(m_list->selectionModel(), &QItemSelectionModel::currentRowChanged,
                this, &B3AssetsPage::updateDetails);
        connect(m_model, &QAbstractItemModel::modelReset, this, &B3AssetsPage::updateDetails);
    }
    columns->addWidget(listCard, 2);

    // Right: selected-asset details and actions.
    auto* detailCard = new QFrame(this);
    B3Theme::markCard(detailCard);
    {
        auto* detailLayout = new QVBoxLayout(detailCard);
        detailLayout->setContentsMargins(B3Theme::kSpaceMd, B3Theme::kSpaceMd, B3Theme::kSpaceMd, B3Theme::kSpaceMd);
        detailLayout->setSpacing(B3Theme::kSpaceSm);

        m_detail_name = new QLabel(detailCard);
        B3Theme::markTextRole(m_detail_name, QStringLiteral("h2"));
        m_detail_name->setTextInteractionFlags(Qt::TextSelectableByMouse);
        detailLayout->addWidget(m_detail_name);

        m_detail_status = new QLabel(detailCard);
        B3Theme::markTextRole(m_detail_status, QStringLiteral("secondary"));
        detailLayout->addWidget(m_detail_status);

        auto addBalanceRow = [&](const QString& title, QLabel** value_out) {
            auto* row = new QHBoxLayout();
            auto* t = new QLabel(title, detailCard);
            B3Theme::markTextRole(t, QStringLiteral("secondary"));
            row->addWidget(t);
            row->addStretch();
            *value_out = makeDetailValue(detailCard);
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
        m_send = new QPushButton(tr("Send"), detailCard);
        m_send->setObjectName(QStringLiteral("assetSend"));
        m_receive = new QPushButton(tr("Receive"), detailCard);
        m_receive->setObjectName(QStringLiteral("assetReceive"));
        actionRow->addWidget(m_send);
        actionRow->addWidget(m_receive);
        detailLayout->addLayout(actionRow);

        auto* meshRow = new QHBoxLayout();
        m_deposit = new QPushButton(tr("Deposit to FlowMesh"), detailCard);
        m_deposit->setObjectName(QStringLiteral("assetDeposit"));
        m_withdraw = new QPushButton(tr("Withdraw from FlowMesh"), detailCard);
        m_withdraw->setObjectName(QStringLiteral("assetWithdraw"));
        meshRow->addWidget(m_deposit);
        meshRow->addWidget(m_withdraw);
        detailLayout->addLayout(meshRow);

        m_backend_note = new QLabel(detailCard);
        B3Theme::markTextRole(m_backend_note, QStringLiteral("secondary"));
        m_backend_note->setWordWrap(true);
        detailLayout->addWidget(m_backend_note);

        m_activity_note = new QLabel(detailCard);
        B3Theme::markTextRole(m_activity_note, QStringLiteral("secondary"));
        m_activity_note->setWordWrap(true);
        detailLayout->addWidget(m_activity_note);

        detailLayout->addStretch();

        connect(m_send, &QPushButton::clicked, this, &B3AssetsPage::sendRequested);
        connect(m_receive, &QPushButton::clicked, this, &B3AssetsPage::receiveRequested);
        // Deposit/withdraw stay disconnected as well as disabled: there is
        // no backend to submit to.
    }
    columns->addWidget(detailCard, 3);

    layout->addLayout(columns, 1);

    updateDetails();
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

    const bool mesh = m_model->source() && m_model->source()->flowMeshAvailable();
    m_deposit->setEnabled(mesh);
    m_withdraw->setEnabled(mesh);
    m_backend_note->setVisible(!mesh);
    m_backend_note->setText(tr("FlowMesh deposits and withdrawals are disabled: no FlowMesh "
                               "backend is available in this build."));

    m_activity_note->setVisible(true);
    m_activity_note->setText(native
        ? tr("Native B3 transactions are listed on the Activity page.")
        : tr("Asset activity requires a coloured-asset backend, which is not "
             "available in this build."));
}
