# Native FAST core probe v1 (TEST ONLY)

This is a healthy fixed-leader, view-zero native core experiment, **not native
P2-FV implementation, a regtest node, or a deployable consensus protocol**.
It tests whether actual auction execution, cryptography and synchronous disk
writes can fit a low local latency budget. It does not claim wallet latency.

## Frozen workload and primary boundary

Four separate C++ replica processes, one separate engine-off C++ client, an
established loopback TCP leader/follower star, fixed generated test committee,
three-of-four certificate including leader, one bounded market with a standing
maker. One original real Schnorr-authenticated order per request. Every order
actually fills one base unit at 100,000 quote atomic units. The fixture uses
existing V1 auction/fee execution, not the V2 shared-accounting or futures model.
Its fee of 10 quote units is checked: seller proceeds 99,990, treasury 2 and four
FN rewards of 2. Synthetic test custody/anchors/keys carry no real value.

Predeclared measurement: two independent fresh campaigns, each two warmups then
40 measured requests, closed-loop/no-load. Record warmups too. No offered-load,
burst, saturation, WAN or arbitrary-load claim. All-replica completion is awaited
between requests. No concurrent model suites/build during timed campaigns.

Primary interval: **before original request signing → original outbox sync →
send → actual execution/certification → client proof verification → client
terminal receipt sync**. Also report proof-verified time separately and initial
submission-to-observed-all-replica-application. The latter is NOT additional
replication delay. Use monotonic clocks; overlapping validator work must not be
summed. JSON logs are emitted after the measured completion, not per event.

No auction execution in the client. The client verifies BLS certification,
original request binding, actual-fill/delta receipt and consecutive state/head
continuity. Its initial INFO head is a trusted isolated-fixture setup, **not** a
production authenticated bootstrap. The certificate binds the exact receipt as
well as entry, original request, sequence, configuration and fixed key set.
The TEST signing domain cannot be used as a V1 checkpoint certificate.

## Durability and refusal

Each replica independently authenticates and executes. It synchronously writes
full intent/evidence **before signing**, then exact signed object **before
publication**. A different candidate cannot replace that intent. Decision,
complete resulting state snapshot and applied head are one synchronous LevelDB
batch before the receipt/ACK. A duplicate certificate cannot apply twice.

Reopen revalidates certificates, re-executes original entries from the generated
fixture, verifies exact resulting snapshot bytes and surviving obligations.
Required missing/malformed evidence refuses. LevelDB owns its process lock; no
lockfile deletion. Stores and all failure evidence are retained. Packet/frame
and generated-request bounds are explicit; this is not untrusted-network DoS
qualification. There is no storage compaction/persistent-state sizing study.

Known quorum/connection failure stops this probe; it has **no native view-change,
coordinator replacement or operational recovery routing**. The Python research
model's RULE-FV is not silently claimed as implemented here. Recovery tests use
process termination and reopening, not physical power loss. Whole coherent old
backup rollback still needs external freshness/fencing. Lost client-response
network recovery is not qualified by the client-reopen check.

## Reproduce

Use a Unix build with the existing node/test prerequisites:

```sh
cmake -S . -B build-native-probe -DBUILD_TESTS=ON -DBUILD_GUI=OFF \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_FLOWMESH_FASTPATH_PROBE=ON
cmake --build build-native-probe --target flowmesh-fastpath-probe -j 6
python3.14 test/flowmesh_v2_fastpath_native/run_probe.py \
  --binary build-native-probe/bin/flowmesh-fastpath-probe \
  --evidence /tmp/b3-native-probe-evidence --interruptions-only
python3.14 test/flowmesh_v2_fastpath_native/run_probe.py \
  --binary build-native-probe/bin/flowmesh-fastpath-probe \
  --evidence /tmp/b3-native-probe-evidence --samples 40 --warmup 2
# Repeat once with the same arguments; a NEW exclusive directory is generated.
```

The target is default-off, separate from the node and not packaged for testers.
Existing node runtime, certificate formats, clients and consensus are untouched.
The launcher records the binary hash, every sample, each child exit, four-store
reopen roots and preserved client originals. Each normal child must exit zero
and emit its clean-shutdown event. Forced cleanup is never a passing shutdown.

Separate `--interruptions-only` checks: cuts after intent, signed vote, publication,
decision batch and in-memory application; exact vote retransmission; duplicate
certificate; six certificate/frame rejection controls; four deliberately broken
generated stores. These direct store tests create the other synthetic signatures
in one process and are **not** the separate-process TCP timing workload.

Stage array in leader `node_us`: execution; intent sync; BLS sign; signed-vote
sync; certificate verify; decision+snapshot sync; memory apply; leader quorum
wait (last field only meaningful for leader). These are locally measured
durations, not a global synchronized event trace.

## Gates before the tester group

Native recovery/view-change and adversarial review; full node/independent-network
integration; unchanged client safety with HTTPS/Qt; real B3 anchor/reconciliation
and continuing regtest block production; load/backlog, disconnect/leader loss,
restart and slow-replica campaigns. No public/regtest-group binaries, mainnet
activation, host delegation or commission flag are provided by this probe.
