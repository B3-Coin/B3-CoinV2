// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_QT_B3FLOWMESHTRADING_H
#define BITCOIN_QT_B3FLOWMESHTRADING_H

#include <qt/b3assettransfer.h>
#include <univalue.h>
#include <QString>
#include <functional>
#include <vector>

namespace B3FlowMeshTrading {
struct Market {
    QString id, base, vault, domain, config, account, checkpoint, reason;
    bool ready{false}, publish_ready{false}, has_account{false}, checkpoint_pending{false};
    uint64_t sequence{0};
    CAmount base_available{0}, base_reserved{0}, b3_available{0}, b3_reserved{0};
};
enum class Operation { Order, Cancel, Deposit, Admit, Withdraw, Checkpoint, Vault };
struct Action {
    Operation operation{Operation::Order};
    Market market;
    QString side, destination, txid;
    CAmount price{0}, amount{0};
    bool native{false};
    uint32_t vout{0};
    UniValue effect;
};
struct Request { std::string method; UniValue params{UniValue::VARR}; };
using RpcCall = std::function<UniValue(const std::string&, const UniValue&)>;

//! All parsers fail closed with std::runtime_error / structured RPC exceptions.
Market ParseMarket(const UniValue& value);
std::vector<Market> ParseMarkets(const UniValue& value);
Market ReadMarket(const QString& id, const RpcCall& rpc);
void CheckSameMarket(const Market& reviewed, const Market& fresh, bool sequence, bool settlement = false);
void CheckSynced(const UniValue& chain);
Request Parameters(const Action& action);
//! No calls at all without approval; cancellation is checked before the write.
UniValue DispatchApproved(const Action& action, bool approved,
                         const std::function<bool()>& cancelled, const RpcCall& rpc);
QString Describe(const Action& action);
//! Validate signed bytes, independently bind every output and actual fee review.
B3AssetTransfer::Prepared ParsePrepared(const UniValue& value, const Action& action,
    const std::function<bool(const CTxDestination&)>& is_spendable);
void CheckActionResult(const UniValue& value, const Action& action);
} // namespace B3FlowMeshTrading
#endif
