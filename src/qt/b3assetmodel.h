// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_B3ASSETMODEL_H
#define BITCOIN_QT_B3ASSETMODEL_H

#include <consensus/amount.h>

#include <QAbstractTableModel>
#include <QList>
#include <QPointer>
#include <QString>

#include <vector>

class WalletModel;
namespace interfaces {
struct WalletAssetBalance;
struct WalletBalances;
} // namespace interfaces

/**
 * The Qt-side representation of one wallet asset. Fields whose backing
 * subsystem does not exist carry an explicit availability flag — the UI
 * renders "not available" rather than a fabricated zero.
 */
struct B3AssetRecord {
    enum class Status {
        Native,      //!< The chain's native coin, backed by real wallet data.
        Active,      //!< An FN or coloured asset from the real wallet backend.
        Unavailable, //!< Known to exist but the backend cannot serve it.
    };

    QString asset_id;
    QString ticker;
    QString display_name;
    CAmount confirmed{0};
    CAmount pending{0};
    CAmount available{0};
    CAmount immature{0};
    CAmount reserved{0};
    CAmount flowmesh{0};
    bool reserved_available{false};
    bool flowmesh_available{false};
    int decimals{8};
    bool metadata_known{false};
    bool is_fn{false};
    bool is_bridge{false};
    Status status{Status::Unavailable};
};

/**
 * Abstract source of asset records. The page and model depend only on
 * this boundary, so replacing B3NativeAssetSource with a real
 * coloured-asset backend requires no view changes.
 */
class B3AssetSource : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    virtual QList<B3AssetRecord> assets() const = 0;
    //! Whether a real coloured-asset backend is connected.
    virtual bool coloredAssetsAvailable() const = 0;
    //! Whether FlowMesh deposit/withdraw are backed by a real interface.
    virtual bool flowMeshAvailable() const = 0;

Q_SIGNALS:
    void assetsChanged();
};

/** Native B3 plus real wallet-owned FN and coloured-asset outputs. */
class B3NativeAssetSource : public B3AssetSource
{
    Q_OBJECT

public:
    explicit B3NativeAssetSource(WalletModel* wallet_model, QObject* parent = nullptr);

    QList<B3AssetRecord> assets() const override;
    bool coloredAssetsAvailable() const override { return true; }
    bool flowMeshAvailable() const override { return false; }

    //! Pure mapping kept public for focused model tests.
    static QList<B3AssetRecord> recordsForBalances(
        const interfaces::WalletBalances& native_balances,
        const std::vector<interfaces::WalletAssetBalance>& asset_balances);

private:
    QPointer<WalletModel> m_wallet_model;
};

/** Table model over a B3AssetSource for the searchable asset list. */
class B3AssetTableModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column { Name = 0, Ticker = 1, Available = 2, ColumnCount = 3 };
    enum Role { SearchRole = Qt::UserRole, AssetIdRole };

    explicit B3AssetTableModel(QObject* parent = nullptr);

    //! Replace the source; previous connections are dropped. May be null.
    void setSource(B3AssetSource* source);
    B3AssetSource* source() const { return m_source; }

    //! The record behind a row (default-constructed if out of range).
    B3AssetRecord recordAt(int row) const;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    //! Fixed-point rendering from integer amounts only; never routes
    //! financial values through floating point.
    static QString formatAmount(CAmount amount, int decimals);

private Q_SLOTS:
    void reload();

private:
    B3AssetSource* m_source{nullptr};
    QList<B3AssetRecord> m_records;
};

#endif // BITCOIN_QT_B3ASSETMODEL_H
