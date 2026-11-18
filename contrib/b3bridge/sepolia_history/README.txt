Public Arbitrum Sepolia history fixtures
======================================

This additive diagnostic collector supplies untrusted L2 availability bytes to
the native public Sepolia verifier. It grants no checkpoint, finality, source
authority, ledger credit or mainnet activation. It does not change the owned
regtest runtime or its journals. All public RPC-free history qualification
flags remain false. There is no Ethereum L1 RPC client, wallet or write method.

Concrete next step
------------------
Take the settled L2 block hash from the native Ethereum-finalized rollup
assertion verification. Acquire one complete block ending at that hash, then
pass the raw header, complete history frame and any ancestor pages back to the
native typed settlement verifier. The provider never selects the trusted hash.
The first milestone needs no custody or endpoint account proofs. If explicitly
requested historical state proofs are unavailable, acquisition fails; the
collector never silently drops them or substitutes latest state.

The default endpoint is literal owned-loopback HTTP and still requires the
public Arbitrum Sepolia chain ID 421614. Chain IDs 412346, 42161, 1337 and 1 refuse. An
existing standalone --dev node cannot be relabelled public Sepolia. Explicit
--allow-public-l2-https permits only reviewed no-key Sepolia L2 hosts:
  https://sepolia-rollup.arbitrum.io/rpc
  https://arbitrum-sepolia.drpc.org
  https://arbitrum-sepolia-rpc.publicnode.com
URL credentials, query keys, redirects, other networks and public HTTP refuse.
The endpoints are availability transports and are never source authorities.

Serialization
-------------
Public L2 APIs commonly do not expose debug/raw-transaction methods. The offline
codec uses the maintained Nitro Geth fork at exact commit
5eee8f546e7602b7deeef7909f291bbebc77e233, including its Nitro transaction types.
No type 0x6a transaction encoding was invented here. Header/transaction/receipt
JSON passes the maintained decoders and consensus serializers; header hash,
every transaction hash, complete transaction/receipt roots, indices and final
cumulative gas must match. This is a structural crypto check; native typed
Ethereum/rollup settlement verification remains required independently.

Build via build_codec.py in an unprivileged systemd scope with enforced 2 GiB
memory, no swap and two CPUs. It refuses an unbounded scope. A local Go proxy
hard-limits ALL new dependency/checksum-log response bytes to 128 MiB, individual
responses to 64 MiB, build disk to 4 GiB and build time to 15 minutes. Existing public
Go cache archives are read-only. Exact fork module and go.mod h1 sums are pinned,
and the maintained Go checksum log verifies dependency downloads. No node,
snapshot service or wallet starts during preparation. Reproduce with a fresh
workspace (the parent directory is the existing private developer-owned build
directory):

  sudo systemd-run --unit=b3-sepolia-codec-build-fresh --uid=josh --pipe --wait --collect \
      --property=MemoryMax=2G --property=MemorySwapMax=0 --property=CPUQuota=200% \
      --property=TasksMax=128 --property=NoNewPrivileges=yes \
      --property=ProtectSystem=strict --property=ProtectHome=read-only \
      --property=ReadWritePaths=/home/josh/B3-tools/sepolia-history-codec-builds \
      --property=PrivateTmp=yes /usr/bin/python3 -B \
      /home/josh/B3-rebirth/contrib/b3bridge/sepolia_history/build_codec.py \
      --workspace /home/josh/B3-tools/sepolia-history-codec-builds/fresh-build \
      --go /home/josh/B3-tools/go1.27.3/go/bin/go \
      --cached-proxy /home/josh/B3-tools/go-modcache/cache/download

Current formatted codec build:
  /home/josh/B3-tools/sepolia-history-codec-builds/20261026-v3/b3-sepolia-history-codec
  /home/josh/B3-tools/sepolia-history-codec-builds/20261026-v3/build-manifest.json
The first v2 build used 75,654,042 new response bytes. The gofmt-formatted v3
rebuild used the already verified local module cache and full checked-in
go.mod/go.sum, in a private network namespace, with ZERO downloaded bytes.
Use --offline with build_codec.py and that prior build's
modules/cache/download directory to reproduce without network access.
Current binary SHA256:
  64c86e9946fbf180a72d99bbb6a6f891957580ee3ae5be697a0564f67f1175ac
The original diagnostic below retains its v2 binary/provenance independently;
no historical artifact was replaced.

Acquisition interface
---------------------
  python3 acquire.py --codec /absolute/b3-sepolia-history-codec \
      --endpoint https://arbitrum-sepolia.drpc.org --allow-public-l2-https \
      --settled-hash 0xHASH_FROM_NATIVE_SETTLEMENT \
      --previous-height N --previous-hash 0xPREDECESSOR \
      --to-height M --to-hash 0xENDPOINT --output /absolute/fresh-output

Optional --accounts accepts a bounded JSON array:
  [{"address":"0x...","storage_keys":["0x..."]}]
The fixed read-only methods are chain ID, block-by-hash/number, complete block
receipts, transaction receipt and eth_getProof. There is no transaction sender,
time override, authentication field or provider-asserted verified input.

One selected range has at most 128 blocks, 8192 records/block and sixteen account
requests with at most 64 exact storage keys. Ancestry uses at most 64 pages of 128
headers. Network replies are capped at 32 MiB, aggregate new fixture responses at 128 MiB,
and acquisition at 300 seconds by default. Raw object aggregate and wire frame are
bounded at 32 MiB. Every output is fresh, fixed-width SHA-addressed where applicable,
and fsynced; no existing evidence is reset.

Outputs:
* manifest.json: kind arbitrum-sepolia, chain 421614, reported untrusted genesis,
  exact requested range/head, endpoint provenance and all authority flags false.
* settled-header.rlp: raw preimage of the requested settled hash.
* history-proof.bin: network-neutral B3ARPR01 with a NEUTRAL assertion, empty L1
  accounts, complete selected blocks newest-first, and optional endpoint state
  witnesses in confirmed_accounts (the newest SELECTED block). It is not an
  owned-regtest source envelope. The typed native public verifier binds network.
* ancestor-pages.json: b3-arbitrum-sepolia-ancestor-pages/1; parent-of-current
  through selected endpoint, at most 128 raw header references per page.
* objects/<sha256>: bounded raw header/transaction/receipt/MPT witness bytes.

Actual diagnostic, 2026-10-26
---------------------------
Public block 322956225, hash
  0x35fca21d5a481808b21a1f3fa15b1c7554fa5e68a636c3ab6278a5696b823aad
was acquired with all seven transactions/receipts, including Nitro type 0x6a.
The maintained serializer and existing offline native complete-history checker
both matched its commitments. Artifact directory:
  /home/josh/B3-evidence/public-sepolia-history-diagnostic-20261026-v2/block
Only 51,964 L2 response bytes, 7 calls and 13,438 raw object bytes were needed.
This diagnostic's externally supplied hash was NOT yet a typed settlement
capability; its native report correctly retains external_anchor_required=true,
rollup_settlement_verified=false and arbitrum_authority=false.

The official and PublicNode endpoints refused historical WETH state proof at
that block. A dRPC request initially returned eight nodes, but a later backend
request also refused historical state. Optional account proof availability is
therefore not qualified. A successful raw block fixture does not qualify public
USDT0 deposit credit or withdrawals.

Actual official snapshot sizing
-------------------------------
snapshot_metadata.py reads ONLY the official small pointer and metadata JSON,
using the same Go HTTP client identity accepted by the official edge. It has no
archive downloader. Previous HTTP 403 responses were client/edge behavior and
were not capacity measurements. On 2026-10-26 the actual published metadata was:

  HashDB pruned, 2026-10-24: 1,941,836,697,600 bytes, compression none, 4 parts.
  PathDB full, 2026-10-20: 2,007,048,622,080 bytes, compression none, 4 parts.
  PathDB archive, 2026-10-13: 2,860,872,581,120 bytes, compression none, 6 parts.
  HashDB archive pointer: 2025-08-12; metadata returned 404, size unqualified.

The laptop had 885,672,312,832 bytes free during the reproduced pruned sizing
check. Even the current pruned archive floor does not fit. Standard download
then extraction has a 3,883,673,395,200-byte peak floor before catch-up/database
overhead. No snapshot part was downloaded and no public Nitro node was started.
This measures the current official bootstrap route; it does not prove a lower
bound for every possible future selective-history implementation.

  python3 snapshot_metadata.py --kind pruned --disk-path /home/josh

Public owned canonical derivation still needs suitable storage and maintained
L1 calldata/blob/history acquisition. An external L2 fixture is a separate,
explicitly RPC-dependent availability/crypto milestone. Mainnet stays disabled.

Checks
------
  python3 -B -m unittest discover -s contrib/b3bridge/sepolia_history -p 'test_*.py'
Eight focused boundary tests cover explicit public opt-in, no-key HTTPS host
scope, writes/control denial, wrong-network relabel refusal before data reads,
duplicate JSON, normalized storage-key duplicates and measured snapshot sizing.
The genuine diagnostic frame also passed the offline native complete-history
checker. End-to-end native public settlement authentication is the next gate.

The suite is registered in the existing CMake offline adapter inventory:

  ctest --test-dir /home/josh/B3-builds/rebirth-20261018T065433Z-xaWC1q \
      -R '^bridge_sepolia_history_adapters$' --output-on-failure

CTest registration passed with bridge/offline labels and a 60-second timeout.
It does not acquire network fixtures, mine faucet work, start services or use
keys. The real cached public fixture additionally passed 11 native CLI cases
in disconnected network namespaces:
  /home/josh/B3-evidence/public-sepolia-history-offline-review-20261026-v1/report.json
The positive complete range retained all seven records. Changed receipt bytes,
changed Nitro transaction bytes, omission of the system transaction/receipt,
changed header, nonneutral assertion, four wrong range cursor variants and an
unproved endpoint account all refused. Every case checked normal native exit
and parsed result scope; crashes are not counted as successful refusals.

Native public integration boundary
----------------------------------
The producer output matches b3-bridge-sepolia-arbitrumcheck's strict ancestry
object fields and neutral complete-history frame. Head proofs are assembled
separately from L1 witnesses: group 0 has 16 accounts and group 1 has 1, with the
same assertion and the producer's settled-header.rlp embedded in both.
Neither proof group contains L2 state witnesses or receipt/transaction data.
Do NOT pass settled-header.rlp as --execution-header: that option requires the
separate Ethereum execution-header preimage authenticated by native Gloas
journal replay. The public verifier must obtain its typed authority in the
same process and bind the rollup assertion, settled L2 header, ancestry and
exact complete selected range before reporting contract settlement.

  b3-bridge-sepolia-arbitrumcheck \
      --policy /absolute/native-policy.json --journal /absolute/native-journal \
      --execution-header /absolute/authenticated-ethereum-header.rlp \
      --head-proof /absolute/l1-group0.bin --head-proof-1 /absolute/l1-group1.bin \
      --ancestor-pages /absolute/fixture/ancestor-pages.json \
      --objects /absolute/fixture/objects \
      --history-proof /absolute/fixture/history-proof.bin \
      --previous-height N --previous-hash 0xPREDECESSOR \
      --to-height M --to-hash 0xENDPOINT

The stored diagnostic used only the standalone commitment checker and has NOT
passed this public typed-settlement gate. Its externally supplied hash is not
an authenticated checkpoint. Even a later successful public crypto check does
not change public_rpc_free_history_qualified=false: this acquisition path still
uses explicitly opted-in public L2 RPC availability. No public stablecoin credit,
public USDT0 withdrawal or production/mainnet readiness follows from it.
