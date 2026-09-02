// Copyright (c) 2021-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <chainparams.h>
#include <key.h>
#include <modern/asset_output.h>
#include <modern/stake.h>
#include <script/sign.h>
#include <util/check.h>
#include <wallet/receive.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/transaction.h>
#include <wallet/wallet.h>

namespace wallet {
namespace {

/**
 * A dry-run signature creator which succeeds only when the exact private key
 * is present, while producing no real signatures or MuSig2 nonce state.
 */
class KeyPresenceSignatureCreator final : public BaseSignatureCreator
{
public:
    const BaseSignatureChecker& Checker() const override { return DUMMY_CHECKER; }

    bool CreateSig(const SigningProvider& provider, std::vector<unsigned char>& signature,
                   const CKeyID& key_id, const CScript& script_code,
                   const SigVersion sigversion) const override
    {
        CKey key;
        if (!provider.GetKey(key_id, key) || !key.IsValid()) return false;
        if (sigversion == SigVersion::WITNESS_V0 && !key.IsCompressed()) {
            return false;
        }
        return DUMMY_SIGNATURE_CREATOR.CreateSig(
            provider, signature, key_id, script_code, sigversion);
    }

    bool CreateSchnorrSig(const SigningProvider& provider,
                          std::vector<unsigned char>& signature,
                          const XOnlyPubKey& pubkey, const uint256* leaf_hash,
                          const uint256* merkle_root,
                          const SigVersion sigversion) const override
    {
        CKey key;
        if (!provider.GetKeyByXOnly(pubkey, key) || !key.IsValid()) return false;
        return DUMMY_SIGNATURE_CREATOR.CreateSchnorrSig(
            provider, signature, pubkey, leaf_hash, merkle_root, sigversion);
    }

    std::vector<uint8_t> CreateMuSig2Nonce(
        const SigningProvider&, const CPubKey&, const CPubKey&, const CPubKey&,
        const uint256*, const uint256*, SigVersion,
        const SignatureData&) const override
    {
        return {};
    }

    bool CreateMuSig2PartialSig(
        const SigningProvider&, uint256&, const CPubKey&, const CPubKey&,
        const CPubKey&, const uint256*,
        const std::vector<std::pair<uint256, bool>>&, SigVersion,
        const SignatureData&) const override
    {
        return false;
    }

    bool CreateMuSig2AggregateSig(
        const std::vector<CPubKey>&, std::vector<uint8_t>&, const CPubKey&,
        const CPubKey&, const uint256*,
        const std::vector<std::pair<uint256, bool>>&, SigVersion,
        const SignatureData&) const override
    {
        return false;
    }
};

const KeyPresenceSignatureCreator KEY_PRESENCE_SIGNATURE_CREATOR;

bool ProviderCanSignScript(const SigningProvider& provider,
                           const CScript& script)
{
    SignatureData signature_data;
    return ProduceSignature(provider, KEY_PRESENCE_SIGNATURE_CREATOR, script,
                            signature_data);
}

} // namespace

bool InputIsMine(const CWallet& wallet, const CTxIn& txin)
{
    AssertLockHeld(wallet.cs_wallet);
    // Ownership of a B3A1 owner suffix depends on the parent transaction's
    // confirmed/mempool provenance. The outpoint-aware overload derives that
    // context and also validates the output index.
    return wallet.IsMine(txin.prevout);
}

bool AllInputsMine(const CWallet& wallet, const CTransaction& tx)
{
    LOCK(wallet.cs_wallet);
    for (const CTxIn& txin : tx.vin) {
        if (!InputIsMine(wallet, txin)) return false;
    }
    return true;
}

CAmount OutputGetCredit(const CWallet& wallet, const CTxOut& txout)
{
    if (!MoneyRange(txout.nValue))
        throw std::runtime_error(std::string(__func__) + ": value out of range");
    LOCK(wallet.cs_wallet);
    return (wallet.IsMine(txout) ? txout.nValue : 0);
}

CAmount TxGetCredit(const CWallet& wallet, const CTransaction& tx)
{
    CAmount nCredit = 0;
    for (const CTxOut& txout : tx.vout)
    {
        nCredit += OutputGetCredit(wallet, txout);
        if (!MoneyRange(nCredit))
            throw std::runtime_error(std::string(__func__) + ": value out of range");
    }
    return nCredit;
}

CScript OutputScriptForWalletContext(
    const CTxOut& txout, const AssetSigningContext asset_context)
{
    // Unlike B3A1, B3S1 needs no era gate for ownership. Its push/drop prefix
    // has the same legacy and modern Script semantics, so the suffix is the
    // actual authorization script even for a pre-H ordinary output.
    if (const auto owner{modern::StakeOwnerScript(txout.scriptPubKey)}) {
        return *owner;
    }
    if (asset_context == AssetSigningContext::OWNER_SUFFIX) {
        if (const auto owner{modern::AssetOwnerScript(txout)}) return *owner;
    }
    return txout.scriptPubKey;
}

bool WalletCanSignScript(const CWallet& wallet, const CScript& script)
{
    AssertLockHeld(wallet.cs_wallet);
    const std::set<ScriptPubKeyMan*> managers{
        wallet.GetScriptPubKeyMans(script)};
    if (managers.empty()) return false;

    // An external signer wallet deliberately has no local secrets. Exact
    // manager ownership is the strongest capability assertion available
    // without prompting the device; foreign scripts were rejected above.
    if (wallet.IsWalletFlagSet(WALLET_FLAG_EXTERNAL_SIGNER)) return true;

    for (const ScriptPubKeyMan* manager : managers) {
        if (const auto* descriptor{
                dynamic_cast<const DescriptorScriptPubKeyMan*>(manager)}) {
            const auto provider{
                descriptor->GetSigningProviderWithPrivateKeys(script)};
            if (provider && ProviderCanSignScript(*provider, script)) return true;
            continue;
        }
        if (const auto* legacy{dynamic_cast<const LegacyDataSPKM*>(manager)};
            legacy && ProviderCanSignScript(*legacy, script)) {
            return true;
        }
    }
    return false;
}

bool ScriptRequiresInactiveB3Witness(
    const CWallet& wallet, const CScript& authorization_script)
{
    AssertLockHeld(wallet.cs_wallet);
    if (!Params().GetConsensus().legacy_b3coin) return false;

    int witness_version{0};
    std::vector<unsigned char> witness_program;
    if (authorization_script.IsWitnessProgram(witness_version,
                                              witness_program)) {
        return true;
    }

    const std::unique_ptr<SigningProvider> provider{
        wallet.GetSolvingProvider(authorization_script)};
    return provider &&
           IsSegWitOutput(*provider, authorization_script,
                          AssetSigningContext::FULL_SCRIPT);
}

bool WalletCanSpendScriptNow(const CWallet& wallet,
                             const CScript& authorization_script)
{
    AssertLockHeld(wallet.cs_wallet);
    return WalletCanSignScript(wallet, authorization_script) &&
           !ScriptRequiresInactiveB3Witness(wallet, authorization_script);
}

bool ScriptIsChange(const CWallet& wallet, const CScript& script)
{
    // TODO: fix handling of 'change' outputs. The assumption is that any
    // payment to a script that is ours, but is not in the address book
    // is change. That assumption is likely to break when we implement multisignature
    // wallets that return change back into a multi-signature-protected address;
    // a better way of identifying which outputs are 'the send' and which are
    // 'the change' will need to be implemented (maybe extend CWalletTx to remember
    // which output, if any, was change).
    AssertLockHeld(wallet.cs_wallet);
    if (wallet.IsMine(script))
    {
        CTxDestination address;
        if (!ExtractDestination(script, address))
            return true;
        if (!wallet.FindAddressBookEntry(address)) {
            return true;
        }
    }
    return false;
}

bool OutputIsChange(const CWallet& wallet, const CTxOut& txout)
{
    return ScriptIsChange(wallet, txout.scriptPubKey);
}

CAmount OutputGetChange(const CWallet& wallet, const CTxOut& txout)
{
    AssertLockHeld(wallet.cs_wallet);
    if (!MoneyRange(txout.nValue))
        throw std::runtime_error(std::string(__func__) + ": value out of range");
    return (OutputIsChange(wallet, txout) ? txout.nValue : 0);
}

CAmount TxGetChange(const CWallet& wallet, const CTransaction& tx)
{
    LOCK(wallet.cs_wallet);
    CAmount nChange = 0;
    for (const CTxOut& txout : tx.vout)
    {
        nChange += OutputGetChange(wallet, txout);
        if (!MoneyRange(nChange))
            throw std::runtime_error(std::string(__func__) + ": value out of range");
    }
    return nChange;
}

static CAmount GetCachableAmount(const CWallet& wallet, const CWalletTx& wtx, CWalletTx::AmountType type, bool avoid_reuse)
{
    auto& amount = wtx.m_amounts[type];
    if (!amount.IsCached(avoid_reuse)) {
        amount.Set(avoid_reuse, type == CWalletTx::DEBIT ? wallet.GetDebit(*wtx.tx) : TxGetCredit(wallet, *wtx.tx));
        wtx.m_is_cache_empty = false;
    }
    return amount.Get(avoid_reuse);
}

CAmount CachedTxGetCredit(const CWallet& wallet, const CWalletTx& wtx, bool avoid_reuse)
{
    AssertLockHeld(wallet.cs_wallet);

    // Must wait until coinbase is safely deep enough in the chain before valuing it
    if (wallet.IsTxImmatureCoinBase(wtx))
        return 0;

    // GetBalance can assume transactions in mapWallet won't change
    return GetCachableAmount(wallet, wtx, CWalletTx::CREDIT, avoid_reuse);
}

CAmount CachedTxGetDebit(const CWallet& wallet, const CWalletTx& wtx, bool avoid_reuse)
{
    if (wtx.tx->vin.empty())
        return 0;

    return GetCachableAmount(wallet, wtx, CWalletTx::DEBIT, avoid_reuse);
}

CAmount CachedTxGetChange(const CWallet& wallet, const CWalletTx& wtx)
{
    if (wtx.fChangeCached)
        return wtx.nChangeCached;
    wtx.nChangeCached = TxGetChange(wallet, *wtx.tx);
    wtx.fChangeCached = true;
    return wtx.nChangeCached;
}

void CachedTxGetAmounts(const CWallet& wallet, const CWalletTx& wtx,
                  std::list<COutputEntry>& listReceived,
                  std::list<COutputEntry>& listSent, CAmount& nFee,
                  bool include_change)
{
    nFee = 0;
    listReceived.clear();
    listSent.clear();

    // Compute fee:
    CAmount nDebit = CachedTxGetDebit(wallet, wtx, /*avoid_reuse=*/false);
    if (nDebit > 0) // debit>0 means we signed/sent this transaction
    {
        CAmount nValueOut = wtx.tx->GetValueOut();
        nFee = nDebit - nValueOut;
    }

    LOCK(wallet.cs_wallet);
    const AssetSigningContext asset_context{
        AssetSigningContextForWalletTransaction(wtx)};
    // Sent/received.
    for (unsigned int i = 0; i < wtx.tx->vout.size(); ++i)
    {
        const CTxOut& txout = wtx.tx->vout[i];
        const CScript reporting_script{
            OutputScriptForWalletContext(txout, asset_context)};
        bool ismine = wallet.IsMine(txout, asset_context);
        // Only need to handle txouts if AT LEAST one of these is true:
        //   1) they debit from us (sent)
        //   2) the output is to us (received)
        if (nDebit > 0)
        {
            if (!include_change && ScriptIsChange(wallet, reporting_script))
                continue;
        }
        else if (!ismine)
            continue;

        // In either case, we need to get the destination address
        CTxDestination address;

        if (!ExtractDestination(reporting_script, address) && !txout.scriptPubKey.IsUnspendable())
        {
            wallet.WalletLogPrintf("CWalletTx::GetAmounts: Unknown transaction type found, txid %s\n",
                                    wtx.GetHash().ToString());
            address = CNoDestination();
        }

        COutputEntry output = {address, txout.nValue, (int)i};

        // If we are debited by the transaction, add the output as a "sent" entry
        if (nDebit > 0)
            listSent.push_back(output);

        // If we are receiving the output, add it as a "received" entry
        if (ismine)
            listReceived.push_back(output);
    }

}

bool CachedTxIsFromMe(const CWallet& wallet, const CWalletTx& wtx)
{
    if (!wtx.m_cached_from_me.has_value()) {
        wtx.m_cached_from_me = wallet.IsFromMe(*wtx.tx);
    }
    return wtx.m_cached_from_me.value();
}

// NOLINTNEXTLINE(misc-no-recursion)
bool CachedTxIsTrusted(const CWallet& wallet, const CWalletTx& wtx, std::set<Txid>& trusted_parents)
{
    AssertLockHeld(wallet.cs_wallet);

    // This wtx is already trusted
    if (trusted_parents.contains(wtx.GetHash())) return true;

    if (wtx.isConfirmed()) return true;
    if (wtx.isBlockConflicted()) return false;
    // using wtx's cached debit
    if (!wallet.m_spend_zero_conf_change || !CachedTxIsFromMe(wallet, wtx)) return false;

    // Don't trust unconfirmed transactions from us unless they are in the mempool.
    if (!wtx.InMempool()) return false;

    // Trusted if all inputs are from us and are in the mempool:
    for (const CTxIn& txin : wtx.tx->vin)
    {
        // Transactions not sent by us: not trusted
        const CWalletTx* parent = wallet.GetWalletTx(txin.prevout.hash);
        if (parent == nullptr) return false;
        // Check that this specific input being spent is trusted
        if (!wallet.IsMine(txin.prevout)) return false;
        // If we've already trusted this parent, continue
        if (trusted_parents.contains(parent->GetHash())) continue;
        // Recurse to check that the parent is also trusted
        if (!CachedTxIsTrusted(wallet, *parent, trusted_parents)) return false;
        trusted_parents.insert(parent->GetHash());
    }
    return true;
}

bool CachedTxIsTrusted(const CWallet& wallet, const CWalletTx& wtx)
{
    std::set<Txid> trusted_parents;
    LOCK(wallet.cs_wallet);
    return CachedTxIsTrusted(wallet, wtx, trusted_parents);
}

Balance GetBalance(const CWallet& wallet, const int min_depth, bool avoid_reuse)
{
    Balance ret;
    bool allow_used_addresses = !avoid_reuse || !wallet.IsWalletFlagSet(WALLET_FLAG_AVOID_REUSE);
    {
        LOCK(wallet.cs_wallet);
        std::set<Txid> trusted_parents;
        for (const auto& [outpoint, txo] : wallet.GetTXOs()) {
            const CWalletTx& wtx = txo.GetWalletTx();

            const bool is_trusted{CachedTxIsTrusted(wallet, wtx, trusted_parents)};
            const int tx_depth{wallet.GetTxDepthInMainChain(wtx)};

            if (!wallet.IsSpent(outpoint) && (allow_used_addresses || !wallet.IsSpentKey(txo.GetTxOut().scriptPubKey))) {
                // Get the amounts for mine
                CAmount credit_mine = txo.GetTxOut().nValue;

                // Set the amounts in the return object
                if (wallet.IsTxImmatureCoinBase(wtx) && wtx.isConfirmed()) {
                    ret.m_mine_immature += credit_mine;
                } else if (is_trusted && tx_depth >= min_depth) {
                    ret.m_mine_trusted += credit_mine;
                } else if (!is_trusted && wtx.InMempool()) {
                    ret.m_mine_untrusted_pending += credit_mine;
                }
            }
        }
    }
    return ret;
}

std::map<CTxDestination, CAmount> GetAddressBalances(const CWallet& wallet)
{
    std::map<CTxDestination, CAmount> balances;

    {
        LOCK(wallet.cs_wallet);
        std::set<Txid> trusted_parents;
        for (const auto& [outpoint, txo] : wallet.GetTXOs()) {
            const CWalletTx& wtx = txo.GetWalletTx();

            if (!CachedTxIsTrusted(wallet, wtx, trusted_parents)) continue;
            if (wallet.IsTxImmatureCoinBase(wtx)) continue;

            int nDepth = wallet.GetTxDepthInMainChain(wtx);
            if (nDepth < (CachedTxIsFromMe(wallet, wtx) ? 0 : 1)) continue;

            CTxDestination addr;
            const AssetSigningContext asset_context{
                AssetSigningContextForWalletTransaction(wtx)};
            if (!wallet.IsMine(txo.GetTxOut(), asset_context)) continue;
            const CScript reporting_script{
                OutputScriptForWalletContext(txo.GetTxOut(), asset_context)};
            if(!ExtractDestination(reporting_script, addr)) continue;

            CAmount n = wallet.IsSpent(outpoint) ? 0 : txo.GetTxOut().nValue;
            balances[addr] += n;
        }
    }

    return balances;
}

std::set< std::set<CTxDestination> > GetAddressGroupings(const CWallet& wallet)
{
    AssertLockHeld(wallet.cs_wallet);
    std::set< std::set<CTxDestination> > groupings;
    std::set<CTxDestination> grouping;

    for (const auto& walletEntry : wallet.mapWallet)
    {
        const CWalletTx& wtx = walletEntry.second;
        const AssetSigningContext asset_context{
            AssetSigningContextForWalletTransaction(wtx)};

        if (wtx.tx->vin.size() > 0)
        {
            bool any_mine = false;
            // group all input addresses with each other
            for (const CTxIn& txin : wtx.tx->vin)
            {
                CTxDestination address;
                if(!InputIsMine(wallet, txin)) /* If this input isn't mine, ignore it */
                    continue;
                const CWalletTx& parent{
                    wallet.mapWallet.at(txin.prevout.hash)};
                const CTxOut& parent_output{
                    parent.tx->vout.at(txin.prevout.n)};
                const CScript parent_script{OutputScriptForWalletContext(
                    parent_output,
                    AssetSigningContextForWalletTransaction(parent))};
                if(!ExtractDestination(parent_script, address))
                    continue;
                grouping.insert(address);
                any_mine = true;
            }

            // group change with input addresses
            if (any_mine)
            {
               for (const CTxOut& txout : wtx.tx->vout) {
                   const CScript reporting_script{
                       OutputScriptForWalletContext(txout, asset_context)};
                   if (ScriptIsChange(wallet, reporting_script))
                   {
                       CTxDestination txoutAddr;
                       if(!ExtractDestination(reporting_script, txoutAddr))
                           continue;
                       grouping.insert(txoutAddr);
                   }
               }
            }
            if (grouping.size() > 0)
            {
                groupings.insert(grouping);
                grouping.clear();
            }
        }

        // group lone addrs by themselves
        for (const auto& txout : wtx.tx->vout)
            if (wallet.IsMine(txout, asset_context))
            {
                CTxDestination address;
                const CScript reporting_script{
                    OutputScriptForWalletContext(txout, asset_context)};
                if(!ExtractDestination(reporting_script, address))
                    continue;
                grouping.insert(address);
                groupings.insert(grouping);
                grouping.clear();
            }
    }

    std::set< std::set<CTxDestination>* > uniqueGroupings; // a set of pointers to groups of addresses
    std::map< CTxDestination, std::set<CTxDestination>* > setmap;  // map addresses to the unique group containing it
    for (const std::set<CTxDestination>& _grouping : groupings)
    {
        // make a set of all the groups hit by this new group
        std::set< std::set<CTxDestination>* > hits;
        std::map< CTxDestination, std::set<CTxDestination>* >::iterator it;
        for (const CTxDestination& address : _grouping)
            if ((it = setmap.find(address)) != setmap.end())
                hits.insert((*it).second);

        // merge all hit groups into a new single group and delete old groups
        std::set<CTxDestination>* merged = new std::set<CTxDestination>(_grouping);
        for (std::set<CTxDestination>* hit : hits)
        {
            merged->insert(hit->begin(), hit->end());
            uniqueGroupings.erase(hit);
            delete hit;
        }
        uniqueGroupings.insert(merged);

        // update setmap
        for (const CTxDestination& element : *merged)
            setmap[element] = merged;
    }

    std::set< std::set<CTxDestination> > ret;
    for (const std::set<CTxDestination>* uniqueGrouping : uniqueGroupings)
    {
        ret.insert(*uniqueGrouping);
        delete uniqueGrouping;
    }

    return ret;
}
} // namespace wallet
