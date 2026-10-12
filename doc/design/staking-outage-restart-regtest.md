# Regtest staking restart after a long outage

The guarded Modern-PoS regtest staking loop can resume block production after a
restart on a fully validated but stale tip. It does not clear initial block
download (IBD), permit finality votes during IBD or change block validity.
Mainnet and the other shipped networks retain their existing behavior.

The original H1-only frozen implementation and test revision is
`66701513201a47d36c06aec260f98f01bf63c6ab`, on `flowmeshV2-dev`, following
`9c7f6a8168b74a5b46af27960c6d6457df0541b5`. These are local commits; this
slice included no push, release, activation or VPS update. The pre-H1 extension
below is a separate local change; the historical results below apply only to
the original H1 profile, not to an unexecuted extension.

## Cause and correction

At startup, a tip older than the normal 24-hour age limit leaves IBD true.
The old staking loop stopped immediately whenever IBD was true. On a halted
chain this formed a circular wait: staking needed a recent block, but staking
was also needed to produce that block.

[CanResumeRegtestStakingAfterOutage](../../src/node/staking.cpp) grants only
local block-production scheduling permission when all these facts hold:

- REGTEST, a pinned legacy boundary and valid Modern PoS parameters for the
  next height. H1-active candidates retain the configured H1 escape rule;
  pre-H1 candidates instead require V1's existing universal eligibility
  saturation at round 128.
- No import or reindex, a fully validated chainstate, a script-validated
  active tip with its body and downloaded ancestry, and sufficient configured
  minimum work.
- The exact best header equals the active tip. A known branch awaiting its
  body cannot satisfy this condition.
- Tip age is genuinely beyond the configured limit, and the applicable
  recovery-round gate has been reached without overflowing height or timestamp.
- A live modern outbound, block-relay or manual block-serving peer has
  recently answered a solicited header query for that exact tip, with no
  outstanding body download from that peer.

The loop repeats the facts when reading the proposer inputs and during the
candidate checks before block signing and submission. Existing eligibility,
assembly, signature verification and `ProcessNewBlock` validation still apply.
Only normal acceptance of a recent block can clear IBD through the unchanged
`UpdateIBDStatus` path. The entire ordinary finality signing and recovery pass
remains behind `!IsInitialBlockDownload()`.

## Peer response boundaries

[RegtestStakingTipSync](../../src/node/regtest_staking_tip_sync.h) holds at
most one request, one response and the last request time per connection.
Only a query queued through the normal GETHEADERS path arms the marker.
Before processing a HEADERS response, the request is consumed. Confirmation
requires normal successful processing of a nonempty, non-full response ending
at the requested exact active tip. Compact-block announcements, invalid or
empty responses and unsolicited headers cannot independently grant permission.

A response must arrive within two minutes of its request. Freshness lasts
five minutes from receipt, not from the end of processing. A clock value earlier
than the recorded observation refuses freshness; smaller clock rollbacks that
remain inside the window are not detected. Disconnect/restart destroys the
connection state.
If initial synchronization already started, the new periodic path can refresh
an expired hint at most once per two minutes per eligible peer, independently
of incoming traffic clearing ordinary download timestamps. This is not a
global rate bound on the pre-existing GETHEADERS paths.

GETHEADERS has no request nonce. A response is an availability hint, not
proof of global synchronization, honest peers, finality or resistance to an
eclipse. One dishonest or isolated peer can omit another branch. Requiring one
such peer is a disclosed local regtest policy, not a new quorum rule or a
production synchronization qualification. Without an eligible peer, recovery
remains safely blocked.

## Executed qualification

This section records only the original H1 qualification at the frozen revision
above. New pre-H1 results must be recorded separately after execution.

The versioned functional profile is `b3-h1-age-only-outage-restart-v1` in
[feature_b3_staking_outage_restart.py](../../test/functional/feature_b3_staking_outage_restart.py).
It uses two disposable local validators with stakes 80/20, minimum finality
set two, H1 at height 132, one-second block/round timing and a synthetic
60-round escape threshold. These are test values, not activation or economic
defaults. The normal tip-age limit is never widened.

The same datadirs and actual signer journals are stopped and reopened using
startup mock time 86,700 seconds after the withheld block's timestamp, already
more than 24 hours after the earlier persisted tip. A genuinely
unfinalized checkpoint is already eligible above the returning signer's vote
watermark. Thus silence during IBD is not a test with nothing available to sign.

| Case | Observed result |
| --- | --- |
| No peer, with staking running | No block or vote; exact journal bytes unchanged |
| Genuine higher modern header with body withheld | No block or vote; journal unchanged |
| Original body delivered but no eligible peer | Still blocked; no journal or watermark change |
| Solicited outbound exact-tip reply | Recovery block accepted normally |
| Same connection after five-minute hint expiry | New query and reply restore the hint |
| After the accepted recovery block | Ordinary IBD exit precedes the next actual finality vote |

The missing-body functional case exercises combined safeguards; the unit
matrix independently refuses a different best header with the peer flag true.
In the final functional run, the persisted height was 145, the signer's
watermark stayed 136 throughout IBD, the eligible checkpoint was 141 and the
watermark reached 141 only after recovery. The withheld original block was
accepted at 146 and the current-time recovery block at 147. Log order verifies
the peer reply, normal IBD exit during block acceptance, production and then
new finality signing. Inclusion in a log is not used as a substitute for the
actual accepted block, journal or signing watermark assertions.

The initial functional case passed with seed `20261011`. The expanded
same-connection renewal case passed with that seed, then passed again on the
rebuilt frozen source with seed `20261012`. The functional test exits zero and
retains a secret-free JSON summary alongside its disposable framework logs.

The negative control restores only the original unconditional IBD gate in the
scratch source, retaining the other repair code. It is not represented as a
pristine baseline build. With the same expanded functional test and seed
`20261011`, both peer probes completed but recovery timed out after 60 seconds:
state `waiting: initial block download`, zero produced blocks and watermark 136.
The control exits one. Its failure trace and binaries remain outside the repo.

The targeted native group passed 42 cases and 2,522 assertions: six new
restart/marker cases, existing download and global IBD policy controls,
proposer scheduling, shipped-network parameters, all signer-store tests, all
fresh-parameter tests and the selected H1/signing-policy boundary cases.
The final rerun against the rebuilt exported frozen tree also passed all
42 cases and 2,522 assertions with seed `20261011` and exit status zero.
This is not a run of every native test or a full V2 qualification.

The macOS arm64 build uses AppleClang 17, RelWithDebInfo, wallet support on and
GUI off. The scratch tree is exported from the frozen revision, excluding the
unrelated unfinished market files and CMake edit. Blob checks of `src`, the
functional framework and build configuration match the frozen tree except
Git's intentional commit-ID expansion in `src/clientversion.cpp`. The scratch
Git metadata still labels the executable `760888dfb5ec-dirty`; that label must
not be mistaken for the tested source revision above.

## Reproduction and remaining gates

From a clean checkout of the frozen revision:

```sh
cmake -S . -B build-staking-outage -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_GUI=OFF -DBUILD_TESTS=ON -DENABLE_WALLET=ON
cmake --build build-staking-outage -j 5 --target b3coind test_bitcoin
build-staking-outage/bin/test_bitcoin \
  '--run_test=finality_signer_store_tests,finality_fresh_params_tests:finality_signing_policy_tests/mainnet_only_default_and_consensus_floor,boundaries_spacing_and_upgrade_watermark,stronger_consensus_invalid_inputs_and_height_limits:finality_h1_tests/h1_knobs_*,h1_knob_values_are_checked,h1_valid_regtest_configuration:modern_pos_tests/regtest_staking_*,regtest_block_download_producer_policy,regtest_finality_carrier_policy_boundaries,mainnet_release_parameters_are_exact,other_shipped_networks_have_no_transition_parameters,parameter_block_is_structurally_valid,preferred_proposer_*,corridor_bits_ruled_value_is_canonical_compact:validation_chainstatemanager_tests/chainstatemanager_ibd_exit_after_loading_blocks' \
  --report_level=detailed --log_level=message --color_output=no --random=20261011
python3 test/functional/feature_b3_staking_outage_restart.py \
  --configfile=build-staking-outage/test/config.ini \
  --nocleanup --randomseed=20261012 --portseed=126
```

The functional test needs permission to bind loopback RPC/P2P ports. An initial
sandbox-denied HTTP bind was an environment failure, not a restart result.
No external chain, Anvil, deployed contract or VPS is used.

A separate read-only source review confirmed the exact frozen commit and
found no concrete defect in this bounded repair. That review executed no
tests and is not production approval. Production synchronization/eclipsing
assumptions and a reviewed activation remain separate gates. The original slice
did not repair pre-H1 or mainnet restart behavior, permanent split locks, missing
historical authority, changing membership, bridge compatibility or machine
power-loss recovery. No journal was deleted and no quorum or certificate rule
was relaxed.

## Preserved pre-H1 regtest extension

An existing regtest datadir may never have configured H1. The producer-only
predicate now covers that exact case without setting a fork height or changing
historical state. The existing V1 `ModernPosEligible` comparison already makes
every positive-weight validator eligible at round 128. Local recovery waits
for that conservative universal bound, in addition to every unchanged context,
body/ancestry, exact-best-header, minimum-work, age and solicited-peer guard.
When H1 is active at the next height, its configured escape rule still applies;
the V1 bound cannot bypass it. A future H1 height is not activated early.

The exact-tip peer query is available for both Modern-PoS profiles. It still
does not prove global synchronization or change IBD, consensus eligibility,
quorum, finality recovery or journal rules. No non-regtest network is enabled.

The explicit functional profile is `b3-pre-h1-age-only-outage-restart-v1`,
selected with `--pre-h1`. It uses the same two disposable 80/20 validators,
minimum finality set two, one-second block/round timing, normal tip-age limit,
real signatures and persisted signer journals as the original profile. It
omits both H1 arguments and asserts the absence of an H1 producer schedule.
Startup mock time models an outage only in this isolated test; operational
recovery must use the real clock. No reset, clock override or imported journal
is part of the repair.

The intended pre-H1 assertions mirror the H1 case: no peer, a withheld body,
or an inbound body alone cannot allow production or signing; a fresh solicited
outbound exact-tip reply can authorize normal V1 production; IBD must exit
normally before any new finality signature. Existing journals are compared
byte-for-byte throughout the blocked phases. The unit matrix independently
covers absent/future H1, the exact round 127/128 boundary, active H1 retaining
a larger escape threshold, missing body/ancestry, imports, insufficient work,
no peer, exact-header mismatch, all non-regtest networks and overflow refusal.

Run both functional profiles against the exact rebuilt tree, separately:

```sh
python3 test/functional/feature_b3_staking_outage_restart.py \
  --configfile=build-staking-outage/test/config.ini \
  --nocleanup --randomseed=20261012 --portseed=127 --pre-h1
python3 test/functional/feature_b3_staking_outage_restart.py \
  --configfile=build-staking-outage/test/config.ini \
  --nocleanup --randomseed=20261012 --portseed=128
```

### Executed successor qualification

The native implementation and functional framework were frozen at
`a82ebbeef618db505834b0275fd0721da285ef8b`. The clean exported build's `src`
and functional tree were compared against that exact commit, excluding the
unrelated unfinished prototype and CMake edit. The macOS arm64 configuration
used AppleClang 17, RelWithDebInfo, wallet support on, GUI and IPC off.

The targeted native group above passed **44 cases and 2,596 assertions** with
seed `20261012` and exit zero. Both functional profiles passed independently
with that seed, port seeds 127 (pre-H1) and 128 (H1), and exit zero. In both,
the persisted height was 145, the signing watermark remained 136 throughout
the blocked phases, and it reached 141 only after the accepted recovery block
and ordinary IBD exit. Both also exercised same-connection probe renewal.
The pre-H1 case explicitly reported no H1 activation or producer schedule.

The negative control used the earlier frozen H1-only daemon, whose native
source is equivalent to `66701513201a47d36c06aec260f98f01bf63c6ab`, with the
new pre-H1 functional profile. No-peer and missing-body safeguards preserved
the original test journals, but the solicited exact-tip probe timed out after
10 seconds because the old networking gate required H1. The test exited one;
its failure is retained, not counted as a passing recovery campaign.

A separate read-only source context inspected the complete six-file repair
and its probe, repeated producer checks, ordinary eligibility/validation and
finality interactions. It found no concrete defect within this bounded
REGTEST-only scope and executed no tests. These results do not qualify
production synchronization, machine power loss, permanent split-lock recovery
or a live network update. Linux build/qualification, exact binary-transition
checks and preserved-server verification remain separate operational gates.
