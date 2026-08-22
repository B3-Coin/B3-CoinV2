// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CORE_IO_H
#define BITCOIN_CORE_IO_H

#include <consensus/amount.h>
#include <util/result.h>

#include <functional>
#include <string>

class CBlock;
namespace Consensus { struct Params; }
class CBlockHeader;
class CScript;
class CTransaction;
struct CMutableTransaction;
class SigningProvider;
class uint256;
class UniValue;
class CTxUndo;
class CTxOut;

/**
 * Verbose level for block's transaction
 */
enum class TxVerbosity {
    SHOW_TXID,                //!< Only TXID for each block's transaction
    SHOW_DETAILS,             //!< Include TXID, inputs, outputs, and other common block's transaction information
    SHOW_DETAILS_AND_PREVOUT  //!< The same as previous option with information about prevouts if available
};

CScript ParseScript(const std::string& s);
std::string ScriptToAsmStr(const CScript& script, bool fAttemptSighashDecode = false);
[[nodiscard]] bool DecodeHexTx(CMutableTransaction& tx, const std::string& hex_tx, bool try_no_witness = false, bool try_witness = true);
[[nodiscard]] bool DecodeHexBlk(CBlock&, const std::string& strHexBlk);
/**
 * Chain-aware overload: on a legacy-B3 chain the block codec is
 * marker-aware (legacy nTime transactions plus the historical trailing
 * signature, or the modern body plus the modern-PoS trailing signature
 * vector); other chains keep the stock witness codec. RPC block
 * submission must use this form on B3 or a modern-PoS block silently
 * loses its validator signature.
 */
[[nodiscard]] bool DecodeHexBlk(CBlock&, const std::string& strHexBlk, const Consensus::Params& params);
bool DecodeHexBlockHeader(CBlockHeader&, const std::string& hex_header);

[[nodiscard]] util::Result<int> SighashFromStr(const std::string& sighash);

UniValue ValueFromAmount(CAmount amount);
std::string FormatScript(const CScript& script);
std::string EncodeHexTx(const CTransaction& tx);
std::string SighashToStr(unsigned char sighash_type);
void ScriptToUniv(const CScript& script, UniValue& out, bool include_hex = true, bool include_address = false, const SigningProvider* provider = nullptr);
void TxToUniv(const CTransaction& tx, const uint256& block_hash, UniValue& entry, bool include_hex = true, const CTxUndo* txundo = nullptr, TxVerbosity verbosity = TxVerbosity::SHOW_DETAILS, std::function<bool(const CTxOut&)> is_change_func = {});

#endif // BITCOIN_CORE_IO_H
