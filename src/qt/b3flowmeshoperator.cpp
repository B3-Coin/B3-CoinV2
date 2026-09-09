// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <qt/b3flowmeshoperator.h>

#include <core_io.h>
#include <crypto/bls.h>
#include <key_io.h>
#include <modern/flowmesh_seat.h>
#include <node/transaction.h>
#include <policy/policy.h>
#include <qt/b3assetmodel.h>
#include <rpc/util.h>
#include <util/strencodings.h>

#include <algorithm>
#include <set>
#include <stdexcept>

namespace B3FlowMeshOperator {
namespace {
[[noreturn]] void Invalid(const char* message) { throw std::runtime_error{message}; }
const UniValue& Field(const UniValue& value, const char* name)
{
    if (!value.isObject()) Invalid("Invalid FN operator response.");
    return value.find_value(name);
}
std::string Hex(const UniValue& value, size_t bytes)
{
    if (!value.isStr() || value.get_str().size() != 2 * bytes || !IsHex(value.get_str())) {
        Invalid("Invalid public identifier in FN operator response.");
    }
    return value.get_str();
}
bool Boolean(const UniValue& value)
{
    if (!value.isBool()) Invalid("Invalid FN operator state.");
    return value.get_bool();
}
int64_t Integer(const UniValue& value)
{
    if (!value.isNum()) Invalid("Invalid FN operator count.");
    const auto result{value.getInt<int64_t>()};
    if (result < 0) Invalid("Invalid negative FN operator count.");
    return result;
}
QStringList Keys(const UniValue& value)
{
    if (!value.isArray() || value.size() > 10000) Invalid("Invalid FN public-key list.");
    QStringList keys;
    for (const auto& key : value.getValues()) {
        const auto hex{Hex(key, bls::PUBKEY_SIZE)};
        if (!bls::PublicKey::Decode(ParseHex(hex))) Invalid("Invalid FN BLS public key.");
        keys.append(QString::fromStdString(hex).toLower());
    }
    const std::set<QString> unique{keys.begin(), keys.end()};
    if (unique.size() != static_cast<size_t>(keys.size())) Invalid("Duplicate FN public key.");
    return keys;
}
} // namespace

Status ParseStatus(const UniValue& value)
{
    const auto& scope{Field(value, "scope")};
    if (!scope.isStr() || scope.get_str() != "node-global") Invalid("Unknown FN worker scope.");
    Status status;
    status.available = Boolean(Field(value, "service_available"));
    status.armed = Boolean(Field(value, "armed"));
    status.fingerprint = QString::fromStdString(Hex(Field(value, "armed_keys_fingerprint"), 32));
    status.wallet_keys = Keys(Field(value, "wallet_bls_pubkeys"));
    status.armed_keys = Keys(Field(value, "armed_bls_pubkeys"));
    for (const auto& key : status.wallet_keys) if (status.armed_keys.contains(key)) ++status.wallet_armed;
    if (Integer(Field(value, "armed_key_count")) != status.armed_keys.size() ||
        Integer(Field(value, "wallet_armed_key_count")) != status.wallet_armed ||
        status.armed != !status.armed_keys.empty()) Invalid("Inconsistent FN armed-key state.");
    const auto& markets{Field(value, "markets")};
    if (!markets.isArray() || markets.size() > 10000) Invalid("Invalid FN market status list.");
    for (const auto& market : markets.getValues()) {
        const QString id{QString::fromStdString(Hex(Field(market, "market_id"), 32))};
        const auto& error{Field(market, "error")};
        const auto& halt{Field(market, "halt")};
        if (!error.isStr() || !halt.isStr()) Invalid("Invalid FN market diagnostics.");
        const bool paused{Boolean(Field(market, "paused"))};
        const bool observer{Boolean(Field(market, "observer_only"))};
        const bool running{Boolean(Field(market, "running"))};
        // Do not manufacture a local signature/finality indicator from a key count.
        const QString state{paused ? QStringLiteral("paused") :
            observer ? QStringLiteral("observer only") :
            running ? QStringLiteral("worker running; signing not independently confirmed") :
            QStringLiteral("not running")};
        QString line{id + QStringLiteral(" — ") + state};
        if (error.isStr() && !error.get_str().empty()) line += QStringLiteral("; ") + QString::fromStdString(error.get_str()).left(300);
        if (halt.isStr() && !halt.get_str().empty()) line += QStringLiteral("; ") + QString::fromStdString(halt.get_str()).left(120);
        status.markets.append(line);
    }
    return status;
}

UniValue ControlParameters(const Status& status)
{
    UniValue params{UniValue::VARR};
    params.push_back(Hex(UniValue{status.fingerprint.toStdString()}, 32));
    return params;
}

UniValue BindParameters()
{
    UniValue params{UniValue::VARR};
    params.push_back(UniValue{}); // Generate an owner address in the captured wallet.
    UniValue options{UniValue::VOBJ};
    options.pushKV("broadcast", false);
    options.pushKV("minconf", 1);
    options.pushKV("include_unsafe", false);
    options.pushKV("replaceable", false);
    params.push_back(options);
    return params;
}

B3AssetTransfer::Prepared ParseBinding(
    const UniValue& value, const B3AssetRecord& fn,
    const std::function<bool(const CTxDestination&)>& is_spendable,
    QString& public_key)
{
    if (!fn.is_fn || !B3AssetTransfer::ValidateAsset(fn).isEmpty() ||
        Boolean(Field(value, "broadcast")) || Boolean(Field(value, "rotation"))) {
        Invalid("Expected a new, unbroadcast binding of one unbound FN Coin.");
    }
    const auto txid{Hex(Field(value, "txid"), 32)};
    if (Hex(Field(value, "seat_txid"), 32) != txid || Integer(Field(value, "seat_vout")) != 0) {
        Invalid("The FN seat does not match the prepared transaction.");
    }
    const auto key_bytes{ParseHex(Hex(Field(value, "bls_pubkey"), bls::PUBKEY_SIZE))};
    const auto key{bls::PublicKey::Decode(key_bytes)};
    const auto& encoded{Field(value, "hex")};
    if (!key || !encoded.isStr() || encoded.get_str().empty() ||
        encoded.get_str().size() > 2 * MAX_STANDARD_TX_WEIGHT || !IsHex(encoded.get_str())) {
        Invalid("Invalid prepared FN binding encoding or public key.");
    }
    CMutableTransaction mutable_tx;
    if (!DecodeHexTx(mutable_tx, encoded.get_str())) Invalid("Cannot decode the prepared FN binding.");
    const CTransaction tx{mutable_tx};
    if (tx.GetHash().GetHex() != txid || tx.IsCoinBase() || tx.vin.empty() || tx.vout.empty() || tx.mpa.size() != 1) {
        Invalid("Unexpected FN binding transaction structure.");
    }
    modern::FlowMeshSeatBindingV1 binding;
    std::string error;
    if (!modern::DecodeFlowMeshSeatBindingRecord(tx.mpa.front(), binding, error) || binding.output_index != 0) {
        Invalid("Invalid FN binding proof record.");
    }
    const auto pop{bls::Signature::Decode(binding.pop)};
    if (!pop || !bls::VerifyPoP(*key, *pop)) Invalid("Invalid FN binding proof of possession.");
    const auto seat{modern::ParseAssetOutput(tx.vout.front())};
    const auto owner{modern::AssetOwnerScript(tx.vout.front())};
    const auto& address{Field(value, "owner_address")};
    if (!seat || !owner || !modern::IsFlowMeshSeatOutput(*seat) || seat->amount != 1 ||
        seat->asset.GetHex() != fn.asset_id.toLower().toStdString() || seat->policy_params != key_bytes ||
        !address.isStr() || GetScriptForDestination(DecodeDestination(address.get_str())) != *owner) {
        Invalid("The FN seat asset, public key or owner differs from the review.");
    }
    for (const auto& input : tx.vin) {
        if (input.prevout.IsNull() || (input.scriptSig.empty() && input.scriptWitness.IsNull())) Invalid("The FN binding is not fully signed.");
    }
    const auto fn_input_txid{Hex(Field(value, "fn_input_txid"), 32)};
    const auto fn_input_vout{Integer(Field(value, "fn_input_vout"))};
    if (std::none_of(tx.vin.begin(), tx.vin.end(), [&](const CTxIn& input) {
        return input.prevout.hash.GetHex() == fn_input_txid && input.prevout.n == fn_input_vout;
    })) Invalid("The FN input is absent from the prepared binding.");
    for (size_t i{0}; i < tx.vout.size(); ++i) {
        const auto& output{tx.vout[i]};
        if (!MoneyRange(output.nValue)) Invalid("Invalid FN binding output value.");
        const auto asset_owner{modern::AssetOwnerScript(output)};
        CTxDestination destination;
        if (!ExtractDestination(asset_owner ? *asset_owner : output.scriptPubKey, destination) ||
            !is_spendable(destination)) Invalid("Every FN binding output must remain owned by the captured wallet.");
        if (i != 0 && modern::ClaimsAssetOutput(output.scriptPubKey)) Invalid("Unexpected extra asset output in FN binding.");
    }
    const CAmount fee{AmountFromValue(Field(value, "network_fee"))};
    if (!MoneyRange(fee) || AmountFromValue(Field(value, "disintegration")) != 0 ||
        fee > node::DEFAULT_MAX_RAW_TX_FEE_RATE.GetFee(GetVirtualTransactionSize(tx))) {
        Invalid("The FN binding has an invalid fee or unexpected B3 destruction.");
    }
    public_key = QString::fromStdString(Hex(Field(value, "bls_pubkey"), bls::PUBKEY_SIZE));
    return {QString::fromStdString(txid), encoded.get_str(), fee, 1, QString::fromStdString(address.get_str())};
}
} // namespace B3FlowMeshOperator
