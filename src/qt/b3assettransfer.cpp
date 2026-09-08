// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <qt/b3assettransfer.h>

#include <core_io.h>
#include <key_io.h>
#include <modern/asset.h>
#include <modern/asset_output.h>
#include <node/transaction.h>
#include <policy/policy.h>
#include <qt/b3assetmodel.h>
#include <rpc/util.h>
#include <univalue.h>
#include <util/strencodings.h>

#include <QUrl>

#include <stdexcept>

namespace B3AssetTransfer {
namespace {

[[noreturn]] void Invalid(const char* message)
{
    throw std::runtime_error{message};
}

bool AssetIdValid(const QString& asset)
{
    const std::string text{asset.toStdString()};
    return text.size() == 64 && IsHex(text) && text != std::string(64, '0');
}

const UniValue& Field(const UniValue& value, const char* name)
{
    if (!value.isObject()) Invalid("The asset backend returned an invalid object.");
    return value.find_value(name);
}

CAmount IntegerField(const UniValue& object, const char* name)
{
    const UniValue& value{Field(object, name)};
    if (!value.isNum()) Invalid("The asset backend returned an invalid integer amount.");
    try {
        const CAmount amount{value.getInt<int64_t>()};
        if (!MoneyRange(amount)) Invalid("The asset backend returned an out-of-range amount.");
        return amount;
    } catch (const std::exception&) {
        Invalid("The asset backend returned an invalid integer amount.");
    }
}

CAmount FeeField(const UniValue& object, const char* name)
{
    try {
        const CAmount amount{AmountFromValue(Field(object, name))};
        if (!MoneyRange(amount)) Invalid("The asset backend returned an invalid B3 fee.");
        return amount;
    } catch (...) {
        Invalid("The asset backend returned an invalid B3 fee.");
    }
}

bool StringEquals(const UniValue& object, const char* name, const QString& value)
{
    const UniValue& field{Field(object, name)};
    return field.isStr() && QString::fromStdString(field.get_str()) == value;
}

} // namespace

std::optional<CAmount> ParseAmount(const QString& text, const int decimals, QString* error)
{
    auto fail = [error](const char* message) -> std::optional<CAmount> {
        if (error) *error = QString::fromLatin1(message);
        return std::nullopt;
    };
    if (error) error->clear();
    if (decimals < 0 || decimals > modern::ASSET_MAX_DECIMALS) {
        return fail("The asset's display precision is invalid.");
    }
    if (text.isEmpty() || text.size() > 64) {
        return fail("Enter a positive amount in plain decimal notation.");
    }
    bool point{false};
    int whole_digits{0};
    int fraction_digits{0};
    CAmount result{0};
    for (const QChar character : text) {
        if (character == QLatin1Char('.') && !point && decimals > 0 && whole_digits > 0) {
            point = true;
            continue;
        }
        if (character < QLatin1Char('0') || character > QLatin1Char('9')) {
            return fail("Use digits and one decimal point; signs, spaces and exponents are not accepted.");
        }
        if (point) {
            if (++fraction_digits > decimals) return fail("The amount has more decimal places than this asset permits.");
        } else {
            ++whole_digits;
        }
        const int digit{character.unicode() - '0'};
        if (result > (MAX_MONEY - digit) / 10) return fail("The amount exceeds the asset limit.");
        result = result * 10 + digit;
    }
    if (whole_digits == 0 || (point && fraction_digits == 0)) return fail("Enter a complete positive amount.");
    for (int i{fraction_digits}; i < decimals; ++i) {
        if (result > MAX_MONEY / 10) return fail("The amount exceeds the asset limit.");
        result *= 10;
    }
    if (result == 0) return fail("The amount must be greater than zero.");
    return result;
}

QString ValidateAsset(const B3AssetRecord& asset)
{
    if (asset.status != B3AssetRecord::Status::Active || !AssetIdValid(asset.asset_id)) {
        return QStringLiteral("Select an active non-native asset with a complete asset ID.");
    }
    if (asset.precision_known && (asset.decimals < 0 || asset.decimals > modern::ASSET_MAX_DECIMALS)) {
        return QStringLiteral("This asset's verified precision is invalid.");
    }
    return {};
}

UniValue PrepareParameters(const B3AssetRecord& asset, CAmount raw_amount, const QString& recipient)
{
    if (!ValidateAsset(asset).isEmpty() || raw_amount <= 0 || !MoneyRange(raw_amount)) {
        Invalid("The asset or amount is invalid.");
    }
    if (recipient.isEmpty() || recipient.size() > 256 ||
        !IsValidDestination(DecodeDestination(recipient.toStdString()))) {
        Invalid("Enter a valid recipient address for this network.");
    }
    UniValue params{UniValue::VARR};
    params.push_back(asset.asset_id.toStdString());
    params.push_back(raw_amount);
    params.push_back(recipient.toStdString());
    UniValue options{UniValue::VOBJ};
    options.pushKV("broadcast", false);
    options.pushKV("minconf", 1);
    options.pushKV("include_unsafe", false);
    options.pushKV("replaceable", false);
    params.push_back(options);
    return params;
}

Prepared ParsePrepared(const UniValue& result, const B3AssetRecord& asset,
                       CAmount raw_amount, const QString& recipient)
{
    // Validate caller input too: helpers must not depend on a particular UI.
    PrepareParameters(asset, raw_amount, recipient);
    const UniValue& broadcast{Field(result, "broadcast")};
    const UniValue& hex{Field(result, "hex")};
    const UniValue& txid{Field(result, "txid")};
    if (!broadcast.isBool() || broadcast.get_bool() || !hex.isStr() ||
        !txid.isStr() || !AssetIdValid(QString::fromStdString(txid.get_str())) ||
        !StringEquals(result, "asset_id", asset.asset_id.toLower()) ||
        !StringEquals(result, "owner_address", recipient) ||
        IntegerField(result, "amount") != raw_amount) {
        Invalid("The prepared transaction does not match the requested asset transfer.");
    }
    const std::string serialized{hex.get_str()};
    if (serialized.empty() || serialized.size() > 2 * MAX_STANDARD_TX_WEIGHT || !IsHex(serialized)) {
        Invalid("The prepared transaction encoding is invalid or too large.");
    }
    CMutableTransaction transaction;
    if (!DecodeHexTx(transaction, serialized) || transaction.vin.empty() || transaction.vout.empty()) {
        Invalid("The prepared transaction cannot be decoded.");
    }
    const CTransaction tx{transaction};
    if (tx.GetHash().GetHex() != txid.get_str() || tx.IsCoinBase() || !tx.mpa.empty()) {
        Invalid("The prepared transaction ID or transfer type is invalid.");
    }
    for (const auto& input : tx.vin) {
        if (input.prevout.IsNull() || (input.scriptSig.empty() && input.scriptWitness.IsNull())) {
            Invalid("The prepared transaction is not fully signed.");
        }
    }
    const auto destination{DecodeDestination(recipient.toStdString())};
    const CScript owner_script{GetScriptForDestination(destination)};
    const auto recipient_asset{modern::ParseAssetOutput(tx.vout.front())};
    const auto recipient_owner{modern::AssetOwnerScript(tx.vout.front())};
    if (!recipient_asset || !recipient_owner ||
        recipient_asset->asset.GetHex() != asset.asset_id.toLower().toStdString() ||
        recipient_asset->amount != raw_amount || *recipient_owner != owner_script) {
        Invalid("The signed recipient output does not match the confirmed asset, amount and address.");
    }
    CAmount asset_total{0};
    for (const auto& output : tx.vout) {
        if (!MoneyRange(output.nValue) ||
            (output.nValue > 0 && (output.scriptPubKey.IsUnspendable() || !output.scriptPubKey.HasValidOps()))) {
            Invalid("Asset sends cannot burn native B3.");
        }
        if (!modern::ClaimsAssetOutput(output.scriptPubKey)) continue;
        const auto parsed{modern::ParseAssetOutput(output)};
        if (!parsed || !modern::AssetOwnerScript(output) ||
            parsed->asset != recipient_asset->asset || parsed->amount > MAX_MONEY - asset_total) {
            Invalid("The prepared transaction contains an unexpected asset output or burn.");
        }
        asset_total += parsed->amount;
    }
    const CAmount change{IntegerField(result, "asset_change")};
    if (change > MAX_MONEY - raw_amount || asset_total != raw_amount + change) {
        Invalid("The prepared transaction's asset change is inconsistent.");
    }
    const CAmount fee{FeeField(result, "network_fee")};
    if (FeeField(result, "disintegration") != 0) Invalid("Asset sends cannot destroy native B3.");
    const CAmount cap{node::DEFAULT_MAX_RAW_TX_FEE_RATE.GetFee(GetVirtualTransactionSize(tx))};
    if (cap <= 0 || fee > cap) Invalid("The prepared B3 fee exceeds the maximum permitted fee rate.");
    return Prepared{QString::fromStdString(txid.get_str()), serialized, fee, raw_amount, recipient};
}

UniValue BroadcastParameters(const Prepared& prepared)
{
    if (prepared.hex.empty() || !IsHex(prepared.hex) || !MoneyRange(prepared.fee)) {
        Invalid("There is no valid prepared transaction to submit.");
    }
    UniValue params{UniValue::VARR};
    params.push_back(prepared.hex);
    params.push_back(ValueFromAmount(node::DEFAULT_MAX_RAW_TX_FEE_RATE.GetFeePerK()));
    params.push_back(ValueFromAmount(0));
    return params;
}

UniValue AcceptanceParameters(const Prepared& prepared)
{
    UniValue transactions{UniValue::VARR};
    transactions.push_back(prepared.hex);
    UniValue params{UniValue::VARR};
    params.push_back(transactions);
    params.push_back(ValueFromAmount(node::DEFAULT_MAX_RAW_TX_FEE_RATE.GetFeePerK()));
    return params;
}

void CheckAcceptance(const UniValue& result, const Prepared& prepared)
{
    if (!result.isArray() || result.size() != 1) Invalid("The transaction preflight result is invalid.");
    const UniValue& entry{result[0]};
    const UniValue& allowed{Field(entry, "allowed")};
    if (!StringEquals(entry, "txid", prepared.txid) || !allowed.isBool() || !allowed.get_bool()) {
        Invalid("The prepared transaction is not currently accepted by this node. No transaction was submitted.");
    }
    if (FeeField(Field(entry, "fees"), "base") != prepared.fee) {
        Invalid("The transaction's verified B3 fee differs from the preparation result.");
    }
}

void CheckChangeOwnership(const Prepared& prepared,
                          const std::function<bool(const CTxDestination&)>& is_spendable)
{
    CMutableTransaction tx;
    if (!DecodeHexTx(tx, prepared.hex) || tx.vout.empty()) Invalid("The prepared change outputs cannot be decoded.");
    for (size_t i{1}; i < tx.vout.size(); ++i) {
        const auto& output{tx.vout[i]};
        const auto owner{modern::AssetOwnerScript(output)};
        const CScript& script{owner ? *owner : output.scriptPubKey};
        CTxDestination destination;
        if (!ExtractDestination(script, destination) || !is_spendable(destination)) {
            Invalid("A prepared change output does not belong to the captured wallet. Nothing was submitted.");
        }
    }
}

void CheckAvailable(const UniValue& result, const B3AssetRecord& asset, CAmount raw_amount)
{
    const UniValue& assets{Field(result, "assets")};
    if (!assets.isArray()) Invalid("The wallet did not return current asset balances.");
    for (const auto& entry : assets.getValues()) {
        if (!StringEquals(entry, "asset_id", asset.asset_id.toLower())) continue;
        if (asset.precision_known) {
            const UniValue& known{Field(entry, "precision_known")};
            if (!known.isBool() || !known.get_bool() || IntegerField(entry, "decimals") != asset.decimals) {
                Invalid("The asset precision changed. Close this dialog and review the asset again.");
            }
        }
        if (IntegerField(entry, "spendable") < raw_amount) {
            Invalid("The unlocked wallet has insufficient mature, safe asset units. B3 is also required for the network fee.");
        }
        return;
    }
    Invalid("The selected asset is no longer available in this wallet.");
}

std::string WalletUri(const QString& wallet_name)
{
    QByteArray encoded{QUrl::toPercentEncoding(wallet_name)};
    if (encoded == "." || encoded == "..") encoded.replace(".", "%2E");
    // Even the unnamed wallet gets an explicit endpoint; never fall back to
    // whichever wallet happens to be globally selected or loaded last.
    return "/wallet/" + std::string{encoded.constData(), static_cast<size_t>(encoded.size())};
}

} // namespace B3AssetTransfer
