// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparamsbase.h>

#include <common/args.h>
#include <tinyformat.h>
#include <util/chaintype.h>

#include <cassert>

void SetupChainParamsBaseOptions(ArgsManager& argsman)
{
    argsman.AddArg("-chain=<chain>", "Use the chain <chain> (default: main). Allowed values: " LIST_CHAIN_NAMES, ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-regtest", "Enter regression test mode, which uses a special chain in which blocks can be solved instantly. "
                 "This is intended for regression testing tools and app development. Equivalent to -chain=regtest.", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-testactivationheight=name@height.", "Set the activation height of 'name' (segwit, bip34, dersig, cltv, csv). (regtest-only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::DEBUG_TEST);
    argsman.AddArg("-b3modernregtest", "Run regtest as a B3 modern-era chain: H=0 (the regtest genesis is the legacy boundary, X = its hash), a temporary-PoW corridor from height 1, and Modern PoS with BLS finality after it, all with scaled scaffolding constants (architecture-contract section 64 activation overrides; regtest-only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::DEBUG_TEST);
    argsman.AddArg("-b3flowmeshtest", "Enable the complete synthetic FN genesis, assets, A2 seat binding and A3 FlowMesh schedule for release qualification (requires -b3modernregtest; regtest-only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::DEBUG_TEST);
    argsman.AddArg("-b3bridgepins=<path>", "Point the synthetic bUSD bridge at a local V1 decentralized deployment and synthetic beacon checkpoint read from this JSON pin file (absolute path; requires -b3modernregtest -b3flowmeshtest; regtest-only)", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION | ArgsManager::DEBUG_ONLY, OptionsCategory::DEBUG_TEST);
    argsman.AddArg("-b3freshlineageheight=<n>", "Start the fresh finality lineage at height <n> (R_f; R_f - 1 must be a V1 checkpoint height) with the staging signer: the V1 single-vote signer also signs the B3/FINALITY/V2 digest under a separate journal (bridge V2 B-7/B-8; requires -b3modernregtest -b3flowmeshtest; regtest-only)", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION | ArgsManager::DEBUG_ONLY, OptionsCategory::DEBUG_TEST);
    argsman.AddArg("-b3freshmaxepochextension=<n>", "Maximum epoch extension in blocks of the fresh finality lineage only (default: 1280; requires -b3freshlineageheight; regtest-only). Not checked against the bridge: choose it so (epoch length + n) blocks at the block interval outlast the Verifier V2 expiry plus three scaled days (regtest plan section 3.3, FD-7), or the lineage breaks before the verifier expires", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION | ArgsManager::DEBUG_ONLY, OptionsCategory::DEBUG_TEST);
    argsman.AddArg("-b3freshkeyheight=<n>", "From height <n> (R_key) the fresh finality lineage seats only independent per-epoch committee BLS keys bound under their own binding kind, never a V1 finality key and never a key bound before (bridge V2 B-37; requires -b3freshlineageheight; 1 <= R_key <= R_f - 2; default: unset, the staging lineage keeps the V1 keys; regtest-only)", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION | ArgsManager::DEBUG_ONLY, OptionsCategory::DEBUG_TEST);
    argsman.AddArg("-b3h1height=<n>", "From height <n> (R_h, above the first modern-PoS height) block producers rotate on a fixed schedule every finality epoch with no certificate needed, using the V1 snapshot, binding and carry-over rules (H1, bridge V2 finality dependency section 5.1; requires -b3modernregtest -b3flowmeshtest; regtest-only)", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION | ArgsManager::DEBUG_ONLY, OptionsCategory::DEBUG_TEST);
    argsman.AddArg("-b3h1escaperounds=<n>", "From R_h a block whose round is at least <n> (R_escape, 1 to 99999) takes its weights from the parent's snapshot and restarts the producer schedule (H1 escape, finality dependency FD-2; default: 2880; requires -b3h1height; regtest-only)", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION | ArgsManager::DEBUG_ONLY, OptionsCategory::DEBUG_TEST);
    argsman.AddArg("-b3h2checkpoint=<height>:<hash>", "Add a hardened modern block checkpoint: the block at <height> must have this hash (H2, finality dependency section 5.3; can be given multiple times; requires -b3modernregtest -b3flowmeshtest; regtest-only)", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION | ArgsManager::DEBUG_ONLY, OptionsCategory::DEBUG_TEST);
    for (const char* knob : {"-b3corridorlength", "-b3corridorspacing", "-b3corridorreward", "-b3blockinterval", "-b3roundseconds", "-b3epochlength", "-b3checkpointinterval", "-b3checkpointdepth", "-b3maxepochextension", "-b3minfinalityset", "-b3reorghorizon", "-b3minstake", "-b3modernreward", "-b3halvinginterval", "-b3treasurypercent", "-b3treasuryscript"}) {
        argsman.AddArg(strprintf("%s=<n>", knob), strprintf("B3 modern-era regtest scaffolding knob (needs -b3modernregtest; regtest-only)"), ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::DEBUG_TEST);
    }
    argsman.AddArg("-testnet", "Use the testnet3 chain. Equivalent to -chain=test. Support for testnet3 is deprecated and will be removed in an upcoming release. Consider moving to testnet4 now by using -testnet4.", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-testnet4", "Use the testnet4 chain. Equivalent to -chain=testnet4.", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-vbparams=deployment:start:end[:min_activation_height]", "Use given start/end times and min_activation_height for specified version bits deployment (regtest-only)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-signet", "Use the signet chain. Equivalent to -chain=signet. Note that the network is defined by the -signetchallenge parameter", ArgsManager::ALLOW_ANY, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-signetchallenge", "Blocks must satisfy the given script to be considered valid (only for signet networks; defaults to the global default signet test network challenge)", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::CHAINPARAMS);
    argsman.AddArg("-signetseednode", "Specify a seed node for the signet network, in the hostname[:port] format, e.g. sig.net:1234 (may be used multiple times to specify multiple seed nodes; defaults to the global default signet test network seed node(s))", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::CHAINPARAMS);
}

static std::unique_ptr<CBaseChainParams> globalChainBaseParams;

const CBaseChainParams& BaseParams()
{
    assert(globalChainBaseParams);
    return *globalChainBaseParams;
}

/**
 * Port numbers for incoming Tor connections (8334, 18334, 38334, 48334, 18445) have
 * been chosen arbitrarily to keep ranges of used ports tight.
 */
std::unique_ptr<CBaseChainParams> CreateBaseChainParams(const ChainType chain)
{
    switch (chain) {
    case ChainType::MAIN:
        return std::make_unique<CBaseChainParams>("", 5467);
    case ChainType::TESTNET:
        return std::make_unique<CBaseChainParams>("testnet3", 18332);
    case ChainType::TESTNET4:
        return std::make_unique<CBaseChainParams>("testnet4", 48332);
    case ChainType::SIGNET:
        return std::make_unique<CBaseChainParams>("signet", 38332);
    case ChainType::REGTEST:
        return std::make_unique<CBaseChainParams>("regtest", 18443);
    }
    assert(false);
}

void SelectBaseParams(const ChainType chain)
{
    globalChainBaseParams = CreateBaseChainParams(chain);
    gArgs.SelectConfigNetwork(ChainTypeToString(chain));
}
