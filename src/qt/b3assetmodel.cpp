// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/b3assetmodel.h>

#include <qt/b3fixed.h>
#include <qt/walletmodel.h>

#include <interfaces/wallet.h>

B3NativeAssetSource::B3NativeAssetSource(WalletModel* wallet_model, QObject* parent)
    : B3AssetSource{parent},
      m_wallet_model{wallet_model}
{
    if (m_wallet_model) {
        connect(m_wallet_model, &WalletModel::balanceChanged, this, &B3AssetSource::assetsChanged);
    }
}

QList<B3AssetRecord> B3NativeAssetSource::assets() const
{
    if (!m_wallet_model) return {};

    const interfaces::WalletBalances balances = m_wallet_model->getCachedBalance();
    B3AssetRecord native;
    native.asset_id = QStringLiteral("native");
    native.ticker = QStringLiteral("B3");
    native.display_name = QStringLiteral("B3");
    native.confirmed = balances.balance;
    native.pending = balances.unconfirmed_balance;
    native.available = balances.balance;
    // No FlowMesh or reservation backend exists in this build; the flags
    // make the UI say so instead of showing a made-up zero.
    native.reserved = 0;
    native.flowmesh = 0;
    native.reserved_available = false;
    native.flowmesh_available = false;
    native.decimals = 8;
    native.metadata_known = true;
    native.status = B3AssetRecord::Status::Native;
    return {native};
}

B3AssetTableModel::B3AssetTableModel(QObject* parent)
    : QAbstractTableModel{parent}
{
}

void B3AssetTableModel::setSource(B3AssetSource* source)
{
    if (m_source) disconnect(m_source, nullptr, this, nullptr);
    m_source = source;
    if (m_source) {
        connect(m_source, &B3AssetSource::assetsChanged, this, &B3AssetTableModel::reload);
        connect(m_source, &QObject::destroyed, this, [this] {
            m_source = nullptr;
            reload();
        });
    }
    reload();
}

void B3AssetTableModel::reload()
{
    beginResetModel();
    m_records = m_source ? m_source->assets() : QList<B3AssetRecord>{};
    endResetModel();
}

B3AssetRecord B3AssetTableModel::recordAt(int row) const
{
    if (row < 0 || row >= m_records.size()) return {};
    return m_records.at(row);
}

int B3AssetTableModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_records.size();
}

int B3AssetTableModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant B3AssetTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_records.size()) return {};
    const B3AssetRecord& record = m_records.at(index.row());

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case Name:
            return record.metadata_known ? record.display_name : tr("Unknown asset");
        case Ticker:
            return record.ticker;
        case Available:
            return formatAmount(record.available, record.decimals);
        }
    }
    if (role == Qt::TextAlignmentRole && index.column() == Available) {
        return QVariant{Qt::AlignRight | Qt::AlignVCenter};
    }
    return {};
}

QVariant B3AssetTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
    case Name: return tr("Asset");
    case Ticker: return tr("Ticker");
    case Available: return tr("Available");
    }
    return {};
}

QString B3AssetTableModel::formatAmount(CAmount amount, int decimals)
{
    // Integer arithmetic only: no float ever touches a financial value.
    return B3Fixed::format(amount, decimals);
}
