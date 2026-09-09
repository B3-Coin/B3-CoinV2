// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/b3assetspage.h>

#include <qt/b3assetsenddialog.h>
#include <qt/b3flowmeshpanel.h>
#include <qt/b3theme.h>
#include <qt/guiutil.h>
#include <qt/walletmodel.h>

#include <interfaces/wallet.h>
#include <key_io.h>
#include <outputtype.h>
#include <util/result.h>

#include <QDialog>
#include <QDialogButtonBox>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSortFilterProxyModel>
#include <QStringList>
#include <QTableView>
#include <QVBoxLayout>

#include <optional>
#include <string>

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
    m_proxy->setFilterRole(B3AssetTableModel::SearchRole);
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
        tr("Wallet-owned B3, FN Coins, and coloured assets appear here. FlowMesh balances appear when its wallet interface is active."),
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
        m_search->setPlaceholderText(tr("Search name, ticker, or paste an asset ID"));
        m_search->setClearButtonEnabled(true);
        listLayout->addWidget(m_search);
        connect(m_search, &QLineEdit::textChanged, this, [this](const QString& text) {
            m_proxy->setFilterFixedString(text.trimmed());
            updateDetails();
        });

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
                this, [this](const QModelIndex& current) {
                    // A reset temporarily moves the view to its first row. Do
                    // not mistake that model-driven move for a user choice.
                    if (m_model_resetting) return;
                    if (current.isValid()) {
                        m_selected_asset_id = current.data(
                            B3AssetTableModel::AssetIdRole).toString();
                    }
                    updateDetails();
                });
        connect(m_model, &QAbstractItemModel::modelAboutToBeReset, this, [this] {
            const QModelIndex current{m_list->currentIndex()};
            if (current.isValid()) {
                m_selected_asset_id = current.data(
                    B3AssetTableModel::AssetIdRole).toString();
            }
            m_model_resetting = true;
        });
        // Wait for the proxy reset, not merely the source reset: only then are
        // proxy rows and their stable asset ids ready to select again.
        connect(m_proxy, &QAbstractItemModel::modelReset, this, [this] {
            m_model_resetting = false;
            updateDetails();
        });
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
        m_detail_name->setObjectName(QStringLiteral("assetName"));
        m_detail_name->setTextFormat(Qt::PlainText);
        B3Theme::markTextRole(m_detail_name, QStringLiteral("h2"));
        m_detail_name->setTextInteractionFlags(Qt::TextSelectableByMouse);
        detailLayout->addWidget(m_detail_name);

        m_detail_status = new QLabel(m_detail_card);
        m_detail_status->setTextFormat(Qt::PlainText);
        m_detail_status->setWordWrap(true);
        m_detail_status->setObjectName(QStringLiteral("assetStatus"));
        B3Theme::markTextRole(m_detail_status, QStringLiteral("status"));
        detailLayout->addWidget(m_detail_status);

        m_detail_id = new QLabel(m_detail_card);
        m_detail_id->setObjectName(QStringLiteral("assetId"));
        m_detail_id->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_detail_id->setWordWrap(true);
        B3Theme::markTextRole(m_detail_id, QStringLiteral("muted"));
        detailLayout->addWidget(m_detail_id);
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
        addBalanceRow(tr("Immature"), &m_detail_immature);
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

        m_action_note = new QLabel(m_detail_card);
        m_action_note->setObjectName(QStringLiteral("assetActionReason"));
        m_action_note->setTextFormat(Qt::PlainText);
        m_action_note->setWordWrap(true);
        B3Theme::markTextRole(m_action_note, QStringLiteral("status"));
        detailLayout->addWidget(m_action_note);

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
        m_backend_note->setObjectName(QStringLiteral("assetFlowMeshReason"));
        m_backend_note->setTextFormat(Qt::PlainText);
        B3Theme::markTextRole(m_backend_note, QStringLiteral("status"));
        m_backend_note->setWordWrap(true);
        detailLayout->addWidget(m_backend_note);

        m_activity_note = new QLabel(m_detail_card);
        B3Theme::markTextRole(m_activity_note, QStringLiteral("muted"));
        m_activity_note->setWordWrap(true);
        detailLayout->addWidget(m_activity_note);

        detailLayout->addStretch();

        connect(m_send, &QPushButton::clicked, this, &B3AssetsPage::sendSelectedAsset);
        connect(m_receive, &QPushButton::clicked, this, &B3AssetsPage::receiveSelectedAsset);
        // The RPC exists, but the production deposit/withdraw UI remains
        // deliberately gated until its complete market lifecycle is tested.
    }
    layout->addLayout(m_columns, 1);
    m_flowmesh_panel = new B3FlowMeshPanel(content);
    layout->addWidget(m_flowmesh_panel);
    connect(this, &B3AssetsPage::walletChanged,
            m_flowmesh_panel, &B3FlowMeshPanel::cancelAndWait);
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
    Q_EMIT walletChanged();
    if (m_wallet_model) disconnect(m_wallet_model, nullptr, this, nullptr);
    m_wallet_model = wallet_model;
    m_flowmesh_panel->setFnAsset({});
    m_flowmesh_panel->setWalletModel(wallet_model);
    if (wallet_model) {
        connect(wallet_model, &WalletModel::canGetAddressesChanged, this, &B3AssetsPage::updateDetails);
        connect(wallet_model, &QObject::destroyed, this, [this] {
            m_wallet_model = nullptr;
            m_have_wallet = false;
            Q_EMIT walletChanged();
            m_flowmesh_panel->setWalletModel(nullptr);
            updateDetails();
        });
    }
    m_have_wallet = wallet_model != nullptr;
    B3AssetSource* old = m_owned_source;
    if (m_have_wallet) {
        m_owned_source = new B3NativeAssetSource(wallet_model, this);
        m_model->setSource(m_owned_source);
    } else {
        m_owned_source = nullptr;
        m_model->setSource(nullptr);
    }
    m_selected_asset_id.clear();
    delete old;
    updateDetails();
}

void B3AssetsPage::setSource(B3AssetSource* source)
{
    Q_EMIT walletChanged();
    if (m_wallet_model) disconnect(m_wallet_model, nullptr, this, nullptr);
    m_wallet_model = nullptr;
    m_flowmesh_panel->setFnAsset({});
    m_flowmesh_panel->setWalletModel(nullptr);
    B3AssetSource* old = m_owned_source;
    m_owned_source = nullptr;
    m_have_wallet = source != nullptr;
    m_model->setSource(source);
    m_selected_asset_id.clear();
    delete old;
    updateDetails();
}

void B3AssetsPage::updateDetails()
{
    // FN operator controls follow the captured wallet's FN record, not the
    // selected trading asset. A view-only test/source must never arm controls.
    B3AssetRecord fn;
    if (m_wallet_model) {
        for (int row{0}; row < m_model->rowCount(); ++row) {
            const auto candidate{m_model->recordAt(row)};
            if (candidate.is_fn) {
                fn = candidate;
                break;
            }
        }
    }
    m_flowmesh_panel->setFnAsset(fn);
    const int rows = m_proxy->rowCount();
    m_empty->setVisible(rows == 0);
    m_empty->setText(m_have_wallet ? tr("No assets to show.") : tr("No wallet is loaded."));

    // Model resets invalidate the view's QModelIndex. Restore the user's
    // selection by stable asset id instead of jumping back to native B3 on
    // every balance refresh.
    if (rows > 0) {
        QModelIndex selected;
        if (!m_selected_asset_id.isEmpty()) {
            for (int row{0}; row < rows; ++row) {
                const QModelIndex candidate{m_proxy->index(row, 0)};
                if (candidate.data(B3AssetTableModel::AssetIdRole).toString() ==
                    m_selected_asset_id) {
                    selected = candidate;
                    break;
                }
            }
        }
        const QModelIndex current{m_list->currentIndex()};
        if (selected.isValid() &&
            (!current.isValid() ||
             current.data(B3AssetTableModel::AssetIdRole).toString() !=
                 m_selected_asset_id)) {
            m_list->setCurrentIndex(selected);
        } else if (!current.isValid()) {
            m_list->setCurrentIndex(m_proxy->index(0, 0));
        }
    }

    B3AssetRecord record;
    bool have_selection = false;
    const QModelIndex current = m_list->currentIndex();
    if (current.isValid()) {
        record = m_model->recordAt(m_proxy->mapToSource(current).row());
        have_selection = !record.asset_id.isEmpty();
        if (have_selection && !m_model_resetting) {
            m_selected_asset_id = record.asset_id;
        }
    }

    if (!have_selection) {
        m_detail_name->setText(tr("No asset selected"));
        m_detail_status->clear();
        m_detail_id->clear();
        for (QLabel* value : {m_detail_confirmed, m_detail_pending, m_detail_available,
                              m_detail_immature, m_detail_reserved, m_detail_flowmesh}) {
            value->setText(QStringLiteral("—"));
        }
        m_send->setEnabled(false);
        m_receive->setEnabled(false);
        m_deposit->setEnabled(false);
        m_withdraw->setEnabled(false);
        const QString reason{!m_security_warning.isEmpty() ? m_security_warning :
            m_have_wallet ? tr("Select an asset to see its available actions.")
                          : tr("Load and select a wallet to use asset actions.")};
        for (QPushButton* button : {m_send, m_receive, m_deposit, m_withdraw}) {
            button->setToolTip(reason);
            button->setAccessibleDescription(reason);
        }
        m_action_note->setText(reason);
        m_action_note->setVisible(true);
        m_backend_note->setVisible(false);
        m_activity_note->setVisible(false);
        return;
    }

    const QString name{B3AssetTableModel::assetName(record)};
    QFontMetrics fm(m_detail_name->font());
    m_detail_name->setText(fm.elidedText(name + QStringLiteral(" (") + record.ticker + QStringLiteral(")"),
                                         Qt::ElideMiddle, 320));
    m_detail_name->setToolTip(QStringLiteral("<qt>%1</qt>")
        .arg(QString{name + QStringLiteral(" (") + record.ticker + QStringLiteral(")")}.toHtmlEscaped()));
    switch (record.status) {
    case B3AssetRecord::Status::Native:
        m_detail_status->setText(tr("Native coin · real wallet balance"));
        break;
    case B3AssetRecord::Status::Active:
        if (record.is_fn) {
            m_detail_status->setText(record.immature > 0
                ? tr("FN Coin · confirmed, waiting for maturity")
                : tr("FN Coin"));
        } else if (record.is_bridge) {
            m_detail_status->setText(tr("Bridged USD · exact six-decimal units"));
        } else if (record.is_test_asset) {
            m_detail_status->setText(tr("Unbacked test asset · %1 decimal places").arg(record.decimals));
        } else if (record.precision_known) {
            m_detail_status->setText(record.metadata_source == QStringLiteral("local-registry")
                ? tr("Local asset label · verified precision (%1 decimals)").arg(record.decimals)
                : tr("Coloured asset · verified precision (%1 decimals)").arg(record.decimals));
        } else {
            m_detail_status->setText(tr("Metadata unavailable · showing exact raw units"));
        }
        break;
    case B3AssetRecord::Status::Unavailable:
        m_detail_status->setText(tr("Backend unavailable"));
        break;
    }
    m_detail_id->setText(record.status == B3AssetRecord::Status::Native
        ? tr("Native B3")
        : tr("Asset ID: %1").arg(record.asset_id));

    const auto amount = [&](CAmount value) { return B3AssetTableModel::formatAmount(value, record.decimals); };
    m_detail_confirmed->setText(amount(record.confirmed));
    m_detail_pending->setText(amount(record.pending));
    m_detail_available->setText(amount(record.available));
    m_detail_immature->setText(amount(record.immature));
    m_detail_reserved->setText(record.reserved_available ? amount(record.reserved) : tr("Not available"));
    m_detail_flowmesh->setText(record.flowmesh_available ? amount(record.flowmesh) : tr("Not available"));

    // Native actions keep their existing pages. Asset sends use a separate
    // exact-unit confirmation flow and never pass through the native B3 form.
    const bool native = record.status == B3AssetRecord::Status::Native;
    const bool signing_wallet{m_wallet_model && !m_wallet_model->wallet().privateKeysDisabled()};
    QString send_reason, receive_reason;
    if (!m_security_warning.isEmpty()) {
        send_reason = receive_reason = m_security_warning;
    } else if (m_action_open) {
        send_reason = receive_reason = tr("Finish or close the current asset dialog first.");
    } else if (!native || !m_have_wallet) {
        if (!m_wallet_model) {
            send_reason = receive_reason = tr("Select a loaded wallet for this asset.");
        } else {
            send_reason = sendAssetDisabledReason(record, signing_wallet);
            if (record.status != B3AssetRecord::Status::Active) {
                receive_reason = tr("This asset's wallet data is unavailable.");
            } else if (!m_wallet_model->wallet().canGetAddresses()) {
                receive_reason = tr("This wallet has no receiving addresses available. Load a wallet that can generate receiving addresses.");
            }
        }
    }
    m_send->setEnabled(send_reason.isEmpty());
    m_receive->setEnabled(receive_reason.isEmpty());
    m_send->setToolTip(send_reason.isEmpty()
        ? (native ? tr("Open the native B3 send page.")
                  : tr("Prepare and review a transfer of this asset. A temporary spending unlock will be requested if needed."))
        : send_reason);
    m_receive->setToolTip(receive_reason.isEmpty()
        ? (native ? tr("Open the native B3 receive page.")
                  : tr("Create a receiving address in the selected wallet. No balance or network fee is required."))
        : receive_reason);
    m_send->setAccessibleDescription(m_send->toolTip());
    m_receive->setAccessibleDescription(m_receive->toolTip());
    QStringList disabled_reasons;
    if (!send_reason.isEmpty()) disabled_reasons.push_back(tr("Send unavailable: %1").arg(send_reason));
    if (!receive_reason.isEmpty()) disabled_reasons.push_back(tr("Receive unavailable: %1").arg(receive_reason));
    m_action_note->setText(disabled_reasons.join(QLatin1Char('\n')));
    m_action_note->setVisible(!disabled_reasons.isEmpty());

    // The model can expose FlowMesh balances, but this page has no approved
    // deposit/withdraw submission path yet. Never turn disconnected buttons
    // into controls that merely look live.
    m_deposit->setEnabled(false);
    m_withdraw->setEnabled(false);
    const QString mesh_reason{tr("FlowMesh deposits and withdrawals remain disabled here pending successful market, deposit and withdrawal testing. "
                                 "Activation height alone does not make a market ready. Deposits enter a keyless vault and may remain locked "
                                 "if the market's validator quorum is unavailable. Trading remains disabled.")};
    for (QPushButton* button : {m_deposit, m_withdraw}) {
        button->setToolTip(mesh_reason);
        button->setAccessibleDescription(mesh_reason);
    }
    m_backend_note->setVisible(true);
    m_backend_note->setText(mesh_reason);

    m_activity_note->setVisible(true);
    m_activity_note->setText(native
        ? tr("Native B3 transactions are listed on the Activity page.")
        : tr("Send transfers only the selected asset; a small native B3 balance pays the network fee. "
             "Receive creates a B3 address and shows the full asset ID for the sender. "
             "Names are labels, not a guarantee of backing."));
}

void B3AssetsPage::showSecurityWarning(const QString& warning)
{
    if (warning.isEmpty()) return;
    // The warning names the captured wallet, which may no longer be selected.
    // Do not clear it on refresh/detach, or permit another unlock on this page.
    if (!m_security_warning.contains(warning)) {
        if (!m_security_warning.isEmpty()) m_security_warning += QLatin1Char('\n');
        m_security_warning += warning;
    }
    m_flowmesh_panel->setEnabled(false);
    updateDetails();
}

bool B3AssetsPage::canSendAsset(const B3AssetRecord& record, const bool signing_wallet)
{
    return sendAssetDisabledReason(record, signing_wallet).isEmpty();
}

QString B3AssetsPage::sendAssetDisabledReason(const B3AssetRecord& record, const bool signing_wallet)
{
    if (record.status != B3AssetRecord::Status::Active || record.asset_id.isEmpty()) {
        return tr("Select an active asset with available wallet data.");
    }
    if (!signing_wallet) return tr("This wallet has no spending keys (watch-only).");
    if (record.confirmed <= record.immature) {
        if (record.immature > 0) return tr("This asset balance is not yet mature. Wait for the required confirmations.");
        if (record.pending > 0) return tr("This asset is awaiting confirmation. Only confirmed asset inputs can be sent.");
        return tr("There is no confirmed, mature balance of this asset to send.");
    }
    // A locked wallet can report available=0 while still owning mature
    // inputs. Let the existing send form request unlock and then recheck its
    // actual spendable balance, precision, transaction and B3 fee.
    return {};
}

B3AssetRecord B3AssetsPage::selectedAsset() const
{
    const QModelIndex current{m_list->currentIndex()};
    return current.isValid() ? m_model->recordAt(m_proxy->mapToSource(current).row()) : B3AssetRecord{};
}

void B3AssetsPage::sendSelectedAsset()
{
    if (m_action_open || !m_security_warning.isEmpty()) return;
    const auto record{selectedAsset()};
    if (record.status == B3AssetRecord::Status::Native && m_have_wallet) {
        Q_EMIT sendRequested();
        return;
    }
    if (!m_wallet_model || !canSendAsset(record, !m_wallet_model->wallet().privateKeysDisabled())) return;
    m_action_open = true;
    updateDetails();
    QPointer<B3AssetsPage> self{this};
    QPointer<B3AssetSendDialog> dialog{new B3AssetSendDialog(m_wallet_model, record, this)};
    connect(this, &B3AssetsPage::walletChanged, dialog, &B3AssetSendDialog::cancelAndWait);
    connect(dialog, &B3AssetSendDialog::securityWarning, this, &B3AssetsPage::showSecurityWarning);
    dialog->exec();
    if (dialog) dialog->deleteLater();
    if (self) {
        m_action_open = false;
        updateDetails();
    }
}

void B3AssetsPage::receiveSelectedAsset()
{
    if (m_action_open || !m_security_warning.isEmpty()) return;
    const auto record{selectedAsset()};
    if (record.status == B3AssetRecord::Status::Native && m_have_wallet) {
        Q_EMIT receiveRequested();
        return;
    }
    const QPointer<WalletModel> wallet_model{m_wallet_model};
    if (!wallet_model || !wallet_model->wallet().canGetAddresses() ||
        record.status != B3AssetRecord::Status::Active) return;
    m_action_open = true;
    updateDetails();
    QPointer<B3AssetsPage> self{this};
    // A normal owner address can receive policy assets. Never invent a token
    // payment URI which an older wallet might interpret as a native B3 send.
    std::optional<CTxDestination> destination;
    std::string destination_error;
    const auto create_destination = [&] {
        const auto result{wallet_model->wallet().getNewDestination(
            OutputType::LEGACY, "Asset receive " + record.asset_id.toStdString())};
        if (result) {
            destination = *result;
        } else {
            destination_error = util::ErrorString(result).original;
        }
    };
    create_destination();
    if (!destination && wallet_model->getEncryptionStatus() == WalletModel::Locked) {
        // A locked wallet can normally use its public keypool. If that pool
        // is exhausted, follow native Receive's temporary-unlock path.
        const auto unlock{wallet_model->requestUnlock()};
        if (!self) return;
        if (!wallet_model || m_wallet_model != wallet_model || !unlock.isValid()) {
            m_action_open = false;
            updateDetails();
            return;
        }
        create_destination();
    }
    if (!destination) {
        QPointer<QMessageBox> error{new QMessageBox(QMessageBox::Warning, tr("Unable to create receive address"),
            QString::fromStdString(destination_error), QMessageBox::Ok, this)};
        error->setTextFormat(Qt::PlainText);
        connect(this, &B3AssetsPage::walletChanged, error, &QDialog::reject);
        error->exec();
        if (error) error->deleteLater();
        if (self) {
            m_action_open = false;
            updateDetails();
        }
        return;
    }
    const QString address{QString::fromStdString(EncodeDestination(*destination))};
    QPointer<QDialog> dialog{new QDialog(this, GUIUtil::dialog_flags)};
    dialog->setWindowTitle(tr("Receive %1").arg(record.ticker));
    dialog->setMinimumWidth(560);
    auto* layout{new QVBoxLayout(dialog)};
    const auto add_text = [&](const QString& text) {
        auto* label{new QLabel(text, dialog)};
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(label);
    };
    add_text(tr("Wallet: %1").arg(QString::fromStdString(wallet_model->wallet().getWalletName())));
    add_text(B3AssetTableModel::assetName(record) + QStringLiteral(" (") + record.ticker + QStringLiteral(")"));
    add_text(tr("Asset ID: %1").arg(record.asset_id));
    add_text(tr("B3 receiving address:"));
    auto* address_field{new QLineEdit(address, dialog)};
    address_field->setObjectName(QStringLiteral("assetReceiveAddress"));
    address_field->setReadOnly(true);
    layout->addWidget(address_field);
    add_text(tr("Give the sender this address AND the full asset ID. They must send the selected asset, "
                "not native B3 or a token on another network. Creating this address spends no coins."));
    if (record.is_test_asset) add_text(tr("Unbacked test asset — no dollar redemption is promised."));
    auto* buttons{new QDialogButtonBox(QDialogButtonBox::Close, dialog)};
    auto* copy_address{buttons->addButton(tr("Copy address"), QDialogButtonBox::ActionRole)};
    auto* copy_asset{buttons->addButton(tr("Copy asset ID"), QDialogButtonBox::ActionRole)};
    connect(copy_address, &QPushButton::clicked, dialog, [address] { GUIUtil::setClipboard(address); });
    connect(copy_asset, &QPushButton::clicked, dialog, [record] { GUIUtil::setClipboard(record.asset_id); });
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(this, &B3AssetsPage::walletChanged, dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog->exec();
    if (dialog) dialog->deleteLater();
    if (self) {
        m_action_open = false;
        updateDetails();
    }
}
