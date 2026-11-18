Owned Nitro development producer
================================

This component runs actual OffchainLabs Nitro v3.12.1 execution on a fresh
standalone development chain. It exports unchanged raw headers, full indexed
transaction/receipt bytes, and EIP-1186 account/storage witnesses. The local
JSON-RPC interface is an availability transport; it is not source authority.

This is NOT Arbitrum One execution, L1 settlement verification, an archive
snapshot, a live USDT0 deployment, or production activation. The native offline
history checker checks commitments against a separately supplied checkpoint;
that checker does not authenticate the checkpoint or create operator facts.

Pinned runtime
--------------

Official image: offchainlabs/nitro-node:v3.12.1-70fa99a
Linux AMD64 image manifest:
sha256:948cf8c42a6ed85bc055f13866fb9a029c8234dc16c12d7ffa4ed669270aaea8
Nitro source: 70fa99a6e711f09eedd96a907290c3df0f3bcf7d
Pinned upstream geth: 5eee8f546e7602b7deeef7909f291bbebc77e233
Actual default dev-chain ID: 412346
Actual default genesis:
0x91d25fc61240927ab960347f3ede23305bcea43d9123601d6d0534df47170daa

The image download is approximately 1.42 GB compressed. The measured initial
Docker/containerd storage on the qualification machine was about 8.14 GB,
plus about 348 MB installed Ubuntu packages. Do not infer full archive storage
needs from this fresh dev chain. The launcher limits its single container to
4 CPUs, 8 GiB memory including swap, 512 PIDs, a read-only root filesystem,
and bounded Docker logs. A private data mount is retained. The serve monitor
checks an 8 GiB runtime-data allowance every two seconds and stops its own
container on failure; this is a monitored allowance, not a filesystem quota.

The dedicated bridge disables IP masquerading. The only published port is
127.0.0.1:18547. Nitro --dev configures no remote parent/provider/feed source.
No existing container, image, volume, journal, or service data is deleted.
The launcher never changes user Docker-group membership or enables mainnet.
Every Docker command uses /usr/bin/docker with the explicit local Unix socket
and a minimal environment; inherited remote contexts and credentials are not
used.

Reproduce on Linux/WSL
---------------------

Install Docker and Compose from the official Ubuntu repositories if absent,
then pull the exact image digest above. Use root Docker CLI rather than adding
the development user to the Docker group. Create a dedicated B3-peer-work
parent directory owned by the development user. From this component directory:

  sudo python3 -B devnet.py start --runtime-parent /path/to/B3-peer-work

The result reports a NEW runtime directory. Existing directories are never
reinitialized. Hold a bounded foreground session while using the node:

  sudo python3 -B devnet.py serve --runtime /path/to/new-runtime --seconds 28800

WSL can stop an idle background Docker service after the initiating session
ends; serve keeps the owned session alive and stops only its own container on
exit. Other terminals can query status and stop it explicitly:

  sudo python3 -B devnet.py status --runtime /path/to/new-runtime
  sudo python3 -B devnet.py stop --runtime /path/to/new-runtime

The official Nitro development account is publicly documented by OffchainLabs
in nitro-devnode/run-dev-node.sh and its local-dev-node guide. Use it only for
owned local fixtures, never for production funds. Normal signed type-2 Foundry
transactions work; blocks include a Nitro system transaction at index zero.
No signer secret is accepted by this component's read-only RPC helper.

Export a block with an explicit account request JSON array containing objects
{address,storage_keys}, with 20-byte addresses and 32-byte storage keys:

  sudo python3 -B export_witness.py --runtime /path/to/runtime \
    --output /path/to/runtime/fresh-export --block-hash 0xEXACT_HASH \
    --accounts /path/to/account-requests.json

The fresh export contains manifest.json plus flat SHA256(raw-byte) object
files. It explicitly reports no native verification or source authority. Use
the sibling arbitrum_history/proof_bundle.py implementation to assemble the
binary B3ARPR01 proof; prepare_range.py adapts a newest-to-oldest export list
with a separately supplied exact range request. It uses neutral assertion
fields and no L1 account witness rather than fabricating rollup settlement:

  python3 -B prepare_range.py --export /path/to/newest-export \
    --export /path/to/oldest-export --source-request /path/to/request.json \
    --output /path/to/fresh-range

The checked Solidity NitroWitnessFixture produces real event/storage changes
on chain 412346 only. Its Deposit/Withdrawn ABI methods are EVENT FIXTURES;
they do not custody ERC20 tokens or prove token backing.

Acceptance
----------

  python3 -B -m unittest discover -s . -p test_devnet_boundary.py -v

The separately supplied genuine fixture at src/test/data/nitro_history_v1 is
checked with the actual native CLI in a disconnected network namespace:

  python3 -B ../arbitrum_history/test_native_proof_cli.py \
    --native /path/to/b3-bridge-arbitrumcheck \
    --fixture /source/src/test/data/nitro_history_v1 \
    --evidence-dir /fresh/path/outside/source

This checker runs 26 actual process cases, including deterministic replay,
complete-root/body checks and adversarial mutation/refusal. Eight kernel
network-denial canaries surround the suite. No portable mock fallback or
skipped test is accepted. Mainnet and Arbitrum authority flags remain false.

Full settlement work remains separate
------------------------------------

The pinned Nitro testnode submodule is d46fe1bfe6dd0dbc2e27dcc6d0dc559a248433f5.
Its default simple configuration permits a staker without a block validator;
that is unsuitable for claiming validated assertions. A future owned full-L1
testnode must use the actual validation process, exact contract/layout pins,
fresh isolated names/ports and resource caps. Do not invoke the upstream
destructive --init path against an existing Docker project. Standalone --dev
does not fill this settlement gap.
