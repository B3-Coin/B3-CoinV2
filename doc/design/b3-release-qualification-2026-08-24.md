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

## §D functional/regtest campaign — EXECUTED (2026-08-24, second pass)

The contract-§64 activation-override facility is implemented
(`-b3modernregtest`, regtest-only: deterministic legacy-format genesis as
the whole legacy era with H = 0 / X = its hash, corridor from height 1,
Modern PoS with scaled scaffolding knobs — the very pins that form the
F = M activation predicate). On it,
`test/functional/feature_b3_finality_soak.py` runs four validator nodes end
to end and **PASSES**: corridor funding → four stakes + bindings (relayed
through the mempool) → Set_0 of four (W 31, quorum 21, no single validator
sufficient) → all four produce and BLS-sign → cross-node `finsig`
aggregation to quorum certificates → epoch rotation on a certified
handover → RPC `invalidateblock` at/below the pin refused
(`modern-finality-violation`) → node restart reproduces finalized state +
pin and resumes signing → a partitioned minority rejoins the majority with
the pin monotone throughout.

The soak exposed and fixed four genuine multi-node defects (commit
`a8290b2`): the phase-blind legacy inv hijack, witness-codec block
re-serialization dropping `vchBlockSig`, BIP152 reconstruction being
structurally unable to carry marker-modern blocks (B3 now announces via
headers and fetches full blocks), and `LoadBlockIndex` losing the persisted
modern-PoS eligibility digest (restarted validators were bricked). None of
these were reachable by single-node unit fixtures — this is exactly what
the campaign existed to find.

## U == U′ equivalence gate

Already **SATISFIED** by the real-history campaign of 2026-08-22 (owner-
authorized sync; see [b3-utxo-equivalence-runs.md](b3-utxo-equivalence-runs.md)):
three-way EQUAL at T1 = 95,350, T2 = 110,000 and T3 = 797,000 with
mutation-negatives verified and artifacts preserved. Remaining is only the
final capture-and-verify **at H itself**, which requires the owner's H pin.

## Cross-platform vector comparison

The frozen vectors are pinned (arm64 canonical). No x86-64 Linux host or
container tooling exists on this machine, so the comparison remains a CI /
owner-machine action: build `test_bitcoin` on x86_64 Linux and run
`finality_qualification_tests`, `finality_types_tests`, `bls_tests`,
`payload_root_tests` — any divergence fails against explicit hex.

## Result

Canonical suite after the soak batch: **206 suites / 188 passed / the same
18 known stock-vector/fixture failures** (`legacy_genesis_tests` was
repaired to the 33-seed release list; see
[b3-test-baseline.md](b3-test-baseline.md)); every finality suite and the
multi-node soak green.
