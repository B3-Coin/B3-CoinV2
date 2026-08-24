# B3 Modern PoS V1 finality — release qualification (2026-08-24)

Plan Commit 19 ([b3-modern-pos-v1-implementation-plan.md](b3-modern-pos-v1-implementation-plan.md) §D)
executed on `test/b3-clean-architecture`. Method: ctest, one suite per process;
canonical baseline carried forward from the pre-batch approvals.

## §D unit matrix → covering suite

| §D requirement | Covered by | Status |
|---|---|---|
| BLS PoP vectors (consensus-spec-tests + ours) | `bls_tests` (Ethereum bls12-381-tests v0.1.2 embedded) | PASS |
| BIP340 binding authorization (valid / wrong key / wrong domain / wrong seq) | `finality_key_tests`, `finality_key_binding_tests` | PASS |
| Aggregate signatures at scale; malformed keys/sigs; infinity; wrong DST | `bls_tests`; n=512 correctness in `finality_qualification_tests` (bench covers 3,500/8,192 timing) | PASS |
| Duplicate BLS key across validators; seq replay/skip/revoke/rebind | `finality_key_tests`, `finality_key_binding_tests` | PASS |
| Snapshot sorting/weights/unbound-excluded/carry-over/MIN_FINALITY_SET | `validator_set_tests`, `finality_epoch_tests`, `finality_eligibility_tests` | PASS |
| Weighted-quorum boundaries (=, −1, one whale) | `finality_certificate_tests`; exact 512-member boundary in `finality_qualification_tests` | PASS |
| Checkpoint timing (interval, depth, epoch-end); delayed {current−1} matrix | `finality_schedule_tests`, `finality_epoch_tests` | PASS |
| Extension + MAX_EPOCH_EXTENSION lineage break; handover failure | `finality_epoch_tests` | PASS |
| Finality pin / forbidden reorg; **persisted pin, fail-closed corruption** | `finality_pin_tests`, `finality_pin_persist_tests` | PASS |
| MPA canonical encoding; unknown/unactivated fail-closed; payload root; txid vs ptxid; cell exclusion | `mpa_tests`, `payload_root_tests`, `ptxid_tests`, `metadata_cell_tests` | PASS |
| Connect/Disconnect state restoration (bindings, epoch, finalized tip) | `finality_epoch_tests` (rebuild equality), `finality_pin_persist_tests` (restart/reindex) | PASS |
| Payload-cost exhaustion + cheap invalid-proof rejection ordering | `payload_cost_tests`, `mpa_tests` | PASS |
| Cross-platform determinism vectors (digest, root, cost) | **`finality_qualification_tests/frozen_cross_platform_vectors`** (this run pins the canonical hex on macOS arm64; a Linux x86_64 CI run must compare) | PASS (arm64) |
| Multi-signer quorum end to end (no single validator at quorum, abstainer, aggregation, inclusion, pin) | **`finality_qualification_tests/three_validator_quorum_end_to_end`** | PASS |
| Multi-byte signer bitmaps / deep zero-padded tree | **`finality_qualification_tests/large_set_and_multibyte_bitmaps`** (n = 12, 512) | PASS |

## §D functional/regtest campaign — status

The plan's `test/functional/feature_b3_finality*.py` campaign needs a
**runtime activation-override facility** (contract §64: regtest/testnet H/X/
modern-PoS overrides via startup options) so an unmodified `bitcoind` can run
a finality chain; that facility is not implemented, and no X is pinned on any
network by design. The unit chain fixtures execute the same scenarios in-
process at scaled constants (activation at M with corridor bindings,
certificate production end to end, missed checkpoint and recovery, handover
at E, extension without quorum, pin refusal across forced reorgs, restart/
-reindex-chainstate persistence, mempool standardness and over-cost
rejection). **Remaining before the X-pin release:** the override facility +
python campaign, and a multi-node `finsig`/binding-tx propagation soak (the
P2P handler has single-node coverage only).

## Result

Canonical suite after the hardening batch + Commit 19: **206 suites / 187
passed / the same 19 known stock-vector/fixture failures** (see
[b3-test-baseline.md](b3-test-baseline.md)); every finality suite green.
