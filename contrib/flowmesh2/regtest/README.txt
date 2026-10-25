B3 REBIRTH: AUTHENTICATED NETWORK REGTEST

The active source is /home/josh/B3-rebirth in Ubuntu-24.04 under WSL.
Windows source view: \\wsl.localhost\Ubuntu-24.04\home\josh\B3-rebirth
The restored backup at C:\development\ON\B3-rebirth is preserved.

Run the complete release gates

  python3 ~/B3-rebirth/contrib/flowmesh2/regtest/qualify_network.py \
    --source-root ~/B3-rebirth \
    --build-dir ~/B3-builds/rebirth-20261018T065433Z-xaWC1q \
    --evidence-dir "$HOME/B3-evidence/network-$(date -u +%Y%m%dT%H%M%SZ)"

Tool and build paths come from the arguments (--tools, --solc, --b3-node-build,
--b3coind, --b3coin-cli, --worker, --asan-build, --tsan-build) or, when an
argument is absent, from FM2_TOOLS, FM2_SOLC, FM2_B3_NODE_BUILD,
FM2_REGTEST_BUILD, FM2_ASAN_BUILD and FM2_TSAN_BUILD (tool_paths.py). A path
given by neither stops the gate with a message naming both.

The evidence directory must be new and outside the source. Failed runs retain
all original state. The runner starts only its own isolated B3 regtest and
unforked Anvil chain31337 processes. There is no mainnet option.

Run the gates on a new machine

Linux x86-64 with the B3 build dependencies, Python 3 and curl. From the
source checkout, fetch the pinned tools (Foundry 1.8.5 and solc 0.8.35; each
download is checked against its SHA-256, nothing is overwritten):

  contrib/flowmesh2/regtest/setup_tools.sh ~/B3-tools
  export FM2_TOOLS=~/B3-tools/foundry-v1.8.5 FM2_SOLC=~/B3-tools/solc-0.8.35

Build three configurations of the same source. The first holds fm2-regtest-node
and the regtest b3coind; the other two carry the sanitizer canaries:

  COMMON="-G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTS=ON
    -DBUILD_FLOWMESH2_FOUNDATION_TESTS=ON -DBUILD_FLOWMESH2=ON -DBUILD_GUI=OFF
    -DBUILD_TX=OFF -DBUILD_UTIL=OFF -DBUILD_WALLET_TOOL=OFF -DENABLE_IPC=OFF
    -DBUILD_BENCH=OFF -DBUILD_FUZZ_BINARY=OFF"
  cmake -S . -B ~/fm2-qual $COMMON -DBUILD_DAEMON=ON -DBUILD_CLI=ON -DENABLE_WALLET=ON
  cmake --build ~/fm2-qual --target fm2_all b3coind b3coin-cli
  cmake -S . -B ~/fm2-qual-asan $COMMON -DBUILD_DAEMON=OFF -DBUILD_CLI=OFF \
    -DENABLE_WALLET=OFF -DSANITIZERS=address,undefined -DFLOWMESH2_SANITIZER_CANARIES=ON
  cmake --build ~/fm2-qual-asan --target fm2_all
  cmake -S . -B ~/fm2-qual-tsan $COMMON -DBUILD_DAEMON=OFF -DBUILD_CLI=OFF \
    -DENABLE_WALLET=OFF -DSANITIZERS=thread -DFLOWMESH2_SANITIZER_CANARIES=ON
  cmake --build ~/fm2-qual-tsan --target fm2_all

Then one command runs every gate:

  export FM2_B3_NODE_BUILD=~/fm2-qual FM2_ASAN_BUILD=~/fm2-qual-asan FM2_TSAN_BUILD=~/fm2-qual-tsan
  python3 contrib/flowmesh2/regtest/qualify_network.py --source-root "$PWD" \
    --build-dir ~/fm2-qual \
    --evidence-dir "$HOME/B3-evidence/network-$(date -u +%Y%m%dT%H%M%SZ)"

Under GCC the ASan canary is built without UBSan's object-size check, so
AddressSanitizer reports first. If TSan aborts with "unexpected memory
mapping", lower vm.mmap_rnd_bits to 28 as the CI workflow does, and only on a
disposable host.

Four operators keep independent consensus, custody, transport and TLS identities.
Three of four signatures are required throughout. A disconnected operator never
reduces quorum. Real encrypted TCP sockets carry votes and application/history
traffic; the fixture coordinator cannot impersonate a sender or repair consensus
by forwarding retained final frames. Pinned TLS1.3 carries signed public actions
and certified account/withdrawal/total proofs. Private maintenance and fault
controls are unavailable through HTTPS.

Signed public orders enter a bounded durable queue before acknowledgment, then
gossip and batch automatically. Confirmed deposits and payouts are discovered
automatically. A public withdrawal gathers independently checked custody votes
and completes through operator-owned gas relayers after its challenge delay.
Normal clients do not collect custody signatures or call private seal/finalize
commands. Operators must fund the separate public gas addresses before service;
the fixture funds them using its own local test accounts.

Network profile fm2-regtest-stable-q/3 uses encoding4, exact-view COMMIT
certificates, and a terminal source-range fact in every batch. This commits the
independently checked source cutoff even when the scan contains no token events.
Earlier /1 and /2 identities, vectors and journals retain their original rules.
Never reopen an old store under /3, truncate journals, or replace lost keys to
make a recovery succeed. Every fresh qualification uses a fresh deployment.

An authenticated quorum-selected view can authorize a different source
observation. The source is independently rechecked and durably recorded before
the worker can resume. Recovery preserves abandoned accepted proposals and
prepared certificates. Historical catchup reexecutes certified contiguous slots
and publishes an ACK only after the state and source checkpoint are durable.
A terminal q3 historical decision can recover an alternate frozen observation
even when that operator missed the final view-change message. It grants no new
voting permission. Both source-handover crash windows have explicit tests.

Custody pins the exact chain, vault, token runtime, proxy implementation (when
configured), deployment, signer epoch, policy and finality rule. Withdrawal
authorizations use EIP712, three independent ECDSA signers, replay protection,
bounded lifetime and exact amount/recipient. The vault reserves backing before
payment, retains failed claims for retry, and enforces a challenge delay. Its
private gas relayer journals the signed transaction before broadcast; ambiguous
retries use the same bytes and nonce. Key rotation requires both rosters and a
delay. Pause preserves obligations. There is no reserve sweep or owner upgrade.
Missing original journals refuse recovery. Journal capacity is explicit in
relayer status and is checked before appending; history is never auto-truncated.

Fees and timing

Q is a synthetic six-decimal quote token, not deployed USDC or USDT. Every
admitted spot market quotes Q. Seller trading fees are 100ppm (0.01%) in Q.
The withdrawal service fee is 1,000 raw Q units (0.001 Q); the operator pays
withdrawal relay gas. Source approve/deposit transactions still need native
gas on their source chain; the local fixture funds those test accounts.

The benchmark executes and verifies 100 real fills after 20 warmup fills. Its
200ms local p95 target excludes source confirmation and withdrawal challenge
waiting. Full client signing/preparation time is reported separately. The local
withdrawal challenge is 30 seconds. Neither local timings nor the generic
contract's provisional delay floors establish live network latency or security.

Required evidence

The network release runner requires the complete earlier regression suite,
every core case and unchanged guard/inventory gate, all custody contracts, real
TLS refusal tests, strict RPC-batch boundary tests, source finality/relayer-codec
tests, same-runtime proxy upgrade refusal, the signed custody roundtrip, actual
network deposit/trade/withdraw and fault recovery, and the network benchmark.
Review qualification-report.json and named gate logs for the exact source and
worker fingerprints. An intermediate standalone pass is not an overall pass.

The same Windows release runner runs the complete core inventory with
AddressSanitizer/UndefinedBehaviorSanitizer and ThreadSanitizer, including
intentional detection controls. Source fingerprints must match the release run.

Limits and remaining activation work

This qualifies synthetic TOKENA, TOKENB and Q against local B3 anchors and an
owned EVM. It does not custody native B3, provide a B3 on-chain exit/checkpoint
proof, or establish live Arbitrum/Ethereum finality. The production finality
policy refuses unpinned or unavailable finalized evidence; local two-confirmation
Anvil tests must never be treated as Arbitrum finality.

Peer discovery uses explicitly signed, pinned numeric endpoints. Internet
deployment, certificate distribution, source-provider qualification, signer
operations, live token/proxy authority, gas economics/replacement policy,
independent cryptographic/security review and sustained adversarial/load
campaigns remain activation gates. No futures or general non-Q fee valuation
are enabled. Mainnet stays disabled even if every local test passes.

Evidence directories contain private test identities and gas keys. Preserve
their permissions; share only the explicitly exported public reports/logs.
Local checksums cannot detect a coherent rollback to an older complete backup.
