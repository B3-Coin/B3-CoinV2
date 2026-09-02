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
        connect(m_wallet_model, &WalletModel::balanceChanged,
                this, &B3AssetSource::assetsChanged);
        connect(m_wallet_model, &WalletModel::assetBalancesChanged,
                this, &B3AssetSource::assetsChanged);
        connect(m_wallet_model, &QObject::destroyed, this, [this] {
            m_wallet_model = nullptr;
            Q_EMIT assetsChanged();
        });
    }
}

QList<B3AssetRecord> B3NativeAssetSource::assets() const
{
    WalletModel* const wallet_model{m_wallet_model.data()};
    if (!wallet_model) return {};

    return recordsForBalances(wallet_model->getCachedBalance(),
                              wallet_model->getCachedAssetBalances());
}

QList<B3AssetRecord> B3NativeAssetSource::recordsForBalances(
    const interfaces::WalletBalances& balances,
    const std::vector<interfaces::WalletAssetBalance>& asset_balances)
{
    QList<B3AssetRecord> records;

    B3AssetRecord native;
    native.asset_id = QStringLiteral("native");
    native.ticker = QStringLiteral("B3");
    native.display_name = QStringLiteral("B3");
    native.confirmed = balances.balance;
    native.pending = balances.unconfirmed_balance;
    native.available = balances.balance;
    native.immature = balances.immature_balance;
    // No FlowMesh or reservation backend exists in this build; the flags
    // make the UI say so instead of showing a made-up zero.
    native.reserved = 0;
    native.flowmesh = 0;
    native.reserved_available = false;
    native.flowmesh_available = false;
    // The locked human-facing B3 denomination is 1 B3 = 1e9 base units.
    native.decimals = 9;
    native.metadata_known = true;
    native.status = B3AssetRecord::Status::Native;
    records.push_back(native);

    for (const interfaces::WalletAssetBalance& balance : asset_balances) {
        B3AssetRecord asset;
        asset.asset_id = QString::fromStdString(balance.asset_id.GetHex());
        asset.confirmed = balance.confirmed;
        asset.pending = balance.unconfirmed;
        asset.available = balance.spendable;
        asset.immature = balance.immature;
        asset.reserved_available = false;
        asset.flowmesh_available = false;
        asset.is_fn = balance.is_fn;
        asset.is_bridge = balance.is_bridge;
        asset.status = B3AssetRecord::Status::Active;

        if (asset.is_fn) {
            asset.ticker = QStringLiteral("FN");
            asset.display_name = tr("FN Coin");
            asset.decimals = 0;
            asset.metadata_known = true;
        } else if (asset.is_bridge) {
            // The bridge identity is consensus-configured metadata. Showing
            // its ticker/precision does not imply that bridge admission is
            // active; it only renders wallet-owned raw units correctly.
            asset.ticker = QStringLiteral("bUSD");
            asset.display_name = tr("Bridged USD");
            asset.decimals = 6;
            asset.metadata_known = true;
        } else {
            // Simple-v1 outputs do not repeat their genesis metadata. Until
            // a metadata registry is available, show exact raw units and a
            // short id rather than inventing a ticker or precision.
            asset.ticker = asset.asset_id.left(8).toUpper();
            asset.display_name = tr("Unknown asset");
            asset.decimals = 0;
            asset.metadata_known = false;
        }
        records.push_back(std::move(asset));
    }

    return records;
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
    if (role == SearchRole) {
        const QString name{record.metadata_known ? record.display_name : tr("Unknown asset")};
        return QString{name + QLatin1Char(' ') + record.ticker + QLatin1Char(' ') +
                       record.asset_id};
    }
    if (role == AssetIdRole) return record.asset_id;
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
