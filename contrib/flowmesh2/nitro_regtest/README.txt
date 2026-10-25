Owned Nitro / genuine fresh USDT0 economic lab
=============================================

Mainnet stays disabled. This is chain412346 with its own genesis, asset ID,
profile and custody vault. It does not certify Arbitrum One or Ethereum/L1
settlement. Source checkpoints are trusted from an owned maintained Nitro full
node. Each of four isolated seat wrappers independently checks full native
header/transaction/receipt/state witnesses before observing and signing. The
direct quorum coordinator is an explicit fixture, not autonomous networking.

Run from the source checkout on Linux/WSL with a fresh evidence directory:
  python3 contrib/flowmesh2/nitro_regtest/run_roundtrip.py \
    --source-root /path/to/source \
    --worker /path/to/fm2-regtest-node --native-checker /path/to/b3-bridge-arbitrumcheck \
    --runtime /path/to/owned-root-nitro-runtime --tools /path/to/foundry \
    --solc /path/to/solc-0.8.35 --evidence-dir /fresh/private/evidence \
    --b3coind /path/to/b3coind --b3coin-cli /path/to/b3coin-cli

The Docker provenance/export helper needs root. Use a newly created root-owned
Nitro runtime with a root-run coordinator, or an already authorized sudo-n
environment. Never change the owner of an existing runtime or signer store.
The pinned maintained node is started separately by contrib/b3bridge/nitro_regtest/devnet.py.

USDT0 implementation and proxy runtime bytes match the reviewed Sourcify
deployments. Fresh constructor initialization and normal owner/OFT-role minting
fund only this local fixture. There is no code/storage overwrite and there is
no external backing claim. TOKENA/B are explicitly synthetic inventory.
Source wallets are fresh code-empty ECDSA EOAs with zero ETH and no approval.
A separate public local development account sponsors native gas. Trading and
withdrawal service fees are accounted only in the profile-bound quote asset.
The vault's challenge interval is 300 real seconds, without a time override.

Private evidence includes generated client/operator identities and exact signed
requests. Publish only the bounded roundtrip-report.json, checked source hashes,
and explicitly curated public artifacts. Never zip an entire evidence directory.
No latency result here qualifies WAN trading or source/L1 finality latency.
