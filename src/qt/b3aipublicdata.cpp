// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.

#include <qt/b3aipublicdata.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

#include <initializer_list>

namespace {
QJsonObject Error()
{
    return {{QStringLiteral("error"), QStringLiteral("Public node data is unavailable or has an unsupported format.")}};
}

void CopyTyped(QJsonObject& out, const QJsonObject& in, QJsonValue::Type type,
               std::initializer_list<const char*> names)
{
    for (const char* name : names) {
        const QString key = QString::fromLatin1(name);
        const QJsonValue value = in.value(key);
        if (value.type() == type) out.insert(key, value);
    }
}

void CopyHashes(QJsonObject& out, const QJsonObject& in,
                std::initializer_list<const char*> names)
{
    for (const char* name : names) {
        const QString key = QString::fromLatin1(name);
        const QJsonValue value = in.value(key);
        if (!value.isString()) continue;
        const QString hash = value.toString();
        if (hash.size() != 64) continue;
        bool valid = true;
        for (const QChar c : hash) {
            const ushort v = c.unicode();
            if (!((v >= '0' && v <= '9') || (v >= 'a' && v <= 'f') || (v >= 'A' && v <= 'F'))) {
                valid = false;
                break;
            }
        }
        if (valid) out.insert(key, hash);
    }
}

QJsonObject PublicSet(const QJsonObject& in)
{
    QJsonObject out;
    CopyTyped(out, in, QJsonValue::Double, {"size", "total_weight", "quorum_weight"});
    CopyHashes(out, in, {"set_hash", "next_set_hash"});
    return out;
}

QJsonObject PublicMarket(const QJsonObject& in)
{
    QJsonObject out;
    CopyHashes(out, in, {"domain", "market_id", "vault_id", "base_asset_id", "execution_config_id",
                         "last_microblock_hash", "state_root", "pending_checkpoint_id"});
    CopyTyped(out, in, QJsonValue::Bool, {"available", "running", "paused", "pending_handoff",
                                        "checkpoint_pending", "genesis_checkpoint_required"});
    CopyTyped(out, in, QJsonValue::Double, {"epoch", "next_microblock_sequence", "next_effect_index", "round",
                                          "pending_checkpoint_sequence", "pending_checkpoint_effect_count"});
    if (!out.contains(QStringLiteral("market_id")) || !out.contains(QStringLiteral("available")) ||
        !out.contains(QStringLiteral("running"))) return {};
    if (in.value(QStringLiteral("quote_asset")).toString() == QStringLiteral("B3")) {
        out.insert(QStringLiteral("quote_asset"), QStringLiteral("B3"));
    }
    const QStringList halts{QStringLiteral("none"), QStringLiteral("invalid-config"), QStringLiteral("store-failure"),
                            QStringLiteral("signing-conflict"), QStringLiteral("certificate-conflict"),
                            QStringLiteral("anchor-invalidated"), QStringLiteral("unavailable"), QStringLiteral("unknown")};
    const QJsonValue halt = in.value(QStringLiteral("halt"));
    if (halt.isString() && halts.contains(halt.toString())) out.insert(QStringLiteral("halt"), halt);
    if (in.value(QStringLiteral("verification")).isObject()) {
        QJsonObject verified;
        CopyTyped(verified, in.value(QStringLiteral("verification")).toObject(), QJsonValue::Bool,
                  {"certificate_verified", "b3_checkpoint_confirmed", "freshest_network_head_proven"});
        if (!verified.isEmpty()) out.insert(QStringLiteral("verification"), verified);
    }
    // MarketStatusJson also contains account balances/identifiers, local queued
    // actions, endpoint URLs and arbitrary errors. Do not copy those fields.
    return out;
}
}

QJsonValue B3AiPublicData::SanitizeResult(const QString& tool, const QJsonValue& raw)
{
    if (tool == QStringLiteral("listflowmeshmarkets")) {
        if (!raw.isArray()) return Error();
        QJsonArray out;
        const QJsonArray markets = raw.toArray();
        if (markets.size() > 128) return Error();
        for (const QJsonValue value : markets) {
            if (!value.isObject()) return Error();
            const QJsonObject market = PublicMarket(value.toObject());
            if (market.isEmpty()) return Error();
            out.append(market);
        }
        return out;
    }

    if (!raw.isObject()) return Error();
    const QJsonObject in = raw.toObject();
    if (in.contains(QStringLiteral("error"))) return Error();
    QJsonObject out;
    if (tool == QStringLiteral("getblockchaininfo")) {
        const QJsonValue chain = in.value(QStringLiteral("chain"));
        const QStringList chains{QStringLiteral("main"), QStringLiteral("test"), QStringLiteral("testnet4"),
                                 QStringLiteral("signet"), QStringLiteral("regtest")};
        if (!chain.isString() || !chains.contains(chain.toString()) || !in.value(QStringLiteral("blocks")).isDouble()) return Error();
        out.insert(QStringLiteral("chain"), chain);
        CopyTyped(out, in, QJsonValue::Double, {"blocks", "headers", "difficulty", "mediantime", "verificationprogress", "pruneheight"});
        CopyTyped(out, in, QJsonValue::Bool, {"initialblockdownload", "pruned"});
        CopyHashes(out, in, {"bestblockhash"});
    } else if (tool == QStringLiteral("getnetworkinfo")) {
        if (!in.value(QStringLiteral("networkactive")).isBool() || !in.value(QStringLiteral("connections")).isDouble()) return Error();
        CopyTyped(out, in, QJsonValue::Double, {"version", "protocolversion", "connections", "connections_in", "connections_out"});
        CopyTyped(out, in, QJsonValue::Bool, {"networkactive", "localrelay"});
        // The RPC also includes configured proxies and advertised addresses.
    } else if (tool == QStringLiteral("getfinalitystatus")) {
        if (!in.value(QStringLiteral("configured")).isBool() || !in.value(QStringLiteral("active")).isBool()) return Error();
        CopyTyped(out, in, QJsonValue::Bool, {"configured", "active", "bootstrapped", "handover_certified", "lineage_broken"});
        CopyTyped(out, in, QJsonValue::Double, {"epoch", "epoch_start"});
        for (const QString& key : {QStringLiteral("finalized"), QStringLiteral("pin")}) {
            if (!in.value(key).isObject()) continue;
            const QJsonObject checkpoint = in.value(key).toObject();
            QJsonObject projected;
            CopyTyped(projected, checkpoint, QJsonValue::Double, {"height", "epoch"});
            CopyHashes(projected, checkpoint, {"hash"});
            if (!projected.isEmpty()) out.insert(key, projected);
        }
        if (in.value(QStringLiteral("validator_set")).isObject()) {
            out.insert(QStringLiteral("validator_set"), PublicSet(in.value(QStringLiteral("validator_set")).toObject()));
        }
        if (in.value(QStringLiteral("set0_preview")).isObject()) {
            const QJsonObject preview = in.value(QStringLiteral("set0_preview")).toObject();
            QJsonObject projected = PublicSet(preview);
            CopyTyped(projected, preview, QJsonValue::Double, {"snapshot_height", "activation_height", "minimum_size"});
            CopyTyped(projected, preview, QJsonValue::Bool, {"ready"});
            out.insert(QStringLiteral("set0_preview"), projected);
        }
        // Local signature-pool contents and signer indices are excluded.
    } else {
        return Error();
    }
    return out;
}
