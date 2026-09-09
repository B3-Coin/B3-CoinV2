// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_B3FLOWMESHOPERATOR_H
#define BITCOIN_QT_B3FLOWMESHOPERATOR_H

#include <qt/b3assettransfer.h>
#include <QStringList>
#include <univalue.h>

struct B3AssetRecord;

namespace B3FlowMeshOperator {
struct Status {
    bool available{false};
    bool armed{false};
    QString fingerprint;
    QStringList wallet_keys;
    QStringList armed_keys;
    int wallet_armed{0};
    QStringList markets;
};

//! Public-key availability is not evidence of a vote or certificate.
Status ParseStatus(const UniValue& value);
UniValue ControlParameters(const Status& status);
UniValue BindParameters();
//! Prepare once; validate the FN carrier, PoP, fee, and captured-wallet owners.
B3AssetTransfer::Prepared ParseBinding(
    const UniValue& value, const B3AssetRecord& fn,
    const std::function<bool(const CTxDestination&)>& is_spendable,
    QString& public_key);
} // namespace B3FlowMeshOperator

#endif // BITCOIN_QT_B3FLOWMESHOPERATOR_H
