// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_B3ASSETTRANSFER_H
#define BITCOIN_QT_B3ASSETTRANSFER_H

#include <addresstype.h>
#include <consensus/amount.h>

#include <QString>

#include <optional>
#include <functional>
#include <string>

class UniValue;
struct B3AssetRecord;

/** Exact, independently testable boundaries for the asset send workflow. */
namespace B3AssetTransfer {

struct Prepared {
    QString txid;
    std::string hex;
    CAmount fee{0};
    CAmount raw_amount{0};
    QString recipient;
};

//! Plain ASCII decimal notation only, bounded by the asset consensus range.
//! Use decimals=0 for explicitly labeled raw-unit entry of unknown precision.
std::optional<CAmount> ParseAmount(const QString& text, int decimals, QString* error = nullptr);
QString ValidateAsset(const B3AssetRecord& asset);

//! All throwing helpers use std::runtime_error with non-secret diagnostics.
UniValue PrepareParameters(const B3AssetRecord& asset, CAmount raw_amount, const QString& recipient);
Prepared ParsePrepared(const UniValue& result, const B3AssetRecord& asset,
                       CAmount raw_amount, const QString& recipient);
UniValue BroadcastParameters(const Prepared& prepared);
UniValue AcceptanceParameters(const Prepared& prepared);
void CheckAcceptance(const UniValue& result, const Prepared& prepared);
//! Every output except the reviewed recipient must be captured-wallet change.
void CheckChangeOwnership(const Prepared& prepared,
                          const std::function<bool(const CTxDestination&)>& is_spendable);

//! A fresh, post-unlock getwalletassets response must cover this amount.
void CheckAvailable(const UniValue& result, const B3AssetRecord& asset, CAmount raw_amount);
std::string WalletUri(const QString& wallet_name);

} // namespace B3AssetTransfer

#endif // BITCOIN_QT_B3ASSETTRANSFER_H
