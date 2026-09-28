# Regtest automatic-producer finality guard

This is a **local production policy**, not a consensus upgrade or repair of
an expired chain. It is enabled by the automatic `StakingLoop` on regtest.
It does not change mainnet finality or block-production rules, validation of received blocks, signer journals,
quorum, certificates, fork choice, or the external bridge contract.

## Failure and prevention

The V1 finality epoch has a nominal length and a bounded extension. If no
current-epoch certificate was included before the extension expires, lineage
becomes broken; blocks can still advance but further certificates are invalid.
Restarting reconstructs the same state.

For epoch start S, length E and extension X, expiry is D=S+E+X. The last legal
certificate carrier is D-1. Stopping only at D is too late if D-1 was empty.

The automatic regtest producer now:

1. Continues its ordinary finality-signing/aggregation/relay pass.
2. Uses the parent-synchronized state projected for the proposed block,
   including any normal epoch rotation.
3. At D-1, returns a template only if handover is already certified or the
   **actual candidate coinbase includes a consensus-judged current-epoch
   certificate**. A previous-epoch certificate is insufficient.
4. Otherwise waits with an explicit reason and no scheduled block time.
5. Retries through the existing scheduling loop; quorum arrival allows the
   same final carrier to be produced without resetting any signing state.
6. Refuses automatic production if lineage is already broken or required
   finality state cannot be reconstructed. It does not clear either condition.

The assembler option is internal and default-off. The automatic regtest loop
enables it; other networks ignore it even if a caller sets the option.
Manual/default assemblers and received blocks keep their original rules.
V1's valid no-bootstrap/no-finality mode remains unchanged. The shared
staking exception path now clears the obsolete scheduled block time on all
networks; this status-only correction does not change production eligibility.

For the observed test incident, D=82,931. Had the guard been active,
participating producers would have waited at tip 82,929 until block 82,930
could include the epoch 412 certificate. This is not a rollback instruction.

### Retained certificates after rollback

A pool can legitimately retain a newer quorum certificate which is now too
shallow for the next block, alongside an older usable certificate. Always
selecting the newest one would turn the producer's safety wait into a needless
stall. The guarded regtest assembler therefore requests candidate-aware
selection: try the existing bounded pool (at most eight slots), using the
unchanged full consensus judge, and choose the first includable certificate.
Rejected candidates and their exact signatures remain in the pool. The
assembler still judges the selected result before including it. Other callers
retain the default selection behavior.

The regression uses a supported operator rollback **only on disposable test
data**, strictly above its sticky finality pin. It does not claim that ordinary
higher-work fork choice spontaneously lowers height, or authorize a rollback
of the preserved installation. Both quorums were accepted before rollback;
the older messages are replayed byte-for-byte, not newly signed below a
watermark. No signer journal is reset to supply them.

## Qualification commands

Build only the native test target in an existing configured build directory:

```sh
cmake --build <build> --target test_bitcoin --parallel 4
<build>/bin/test_bitcoin --run_test=modern_pos_tests/regtest_finality_carrier_policy_boundaries:finality_production_tests/regtest_staking_preserves_last_handover_carrier,regtest_carrier_rejects_previous_epoch_and_broken_lineage,regtest_carrier_uses_older_valid_pooled_certificate --report_level=detailed --log_level=message --color_output=no
```

The expensive native fixtures mine a generated legacy prefix before their
modern test chain. This setup time is not a trading-latency measurement.
Tests cover last-carrier boundaries, projected rotation, non-regtest bypass,
wide arithmetic, missing state, actual certificate inclusion, previous-epoch
and bad/insufficient signatures, same-journal staking-loop restart, delayed
quorum, tracker replay, explicit refusal on already-broken lineage, and
candidate-aware selection after a permitted rollback.

The original consensus expiry tests remain unchanged. Their passing is
expected: a manually built empty expiry-crossing block remains valid under
V1. The local policy must not be described as network-wide enforcement.

## Guarantees not made

- Other producers can still send valid empty blocks beyond the local guard.
- A partition or insufficient quorum can leave production waiting forever.
- Preserving the final carrier does not make incompatible old votes compatible.
- The catch-up guard is separate and also has a withheld-body availability limit.
- A staking-loop restart is not a daemon crash or power-loss recovery test.
- No wallet/funds migration, new test network, or production activation occurs.

## Recovery of the preserved incident: unresolved authority

The retained test chain already passed its expiry. Two validators retain
different orphaned ancestry locks. The other two have weight 30/100 and two
signers, below both thresholds (67 weight and three signers). Every possible
quorum needs the weight 40 validator, whose own orphan lock remains binding.

Two distinct problems therefore need distinct evidence:

1. **Certificate acceptance:** admitting a late certificate would extend an
   expired validator set's authority. This needs an explicit consensus and
   security decision, preserving all domain, exact-set, successor, ancestry,
   withdrawal-root, depth, monotonicity, bitmap, quorum and BLS checks.
2. **Certificate creation:** permitting a signer to leave an orphaned lock
   needs proof or explicitly accepted trust. A late-acceptance rule cannot
   create that authority. A newer height, majority of connected peers, deeper
   burial, or an empty current pool does not revoke old V1 signatures.

The existing real-BLS counterexample in
[`finality_depth_recovery_prototype_tests.cpp`](../src/test/finality_depth_recovery_prototype_tests.cpp)
demonstrates that an ancestry fallback can leave two incompatible quorum
certificates valid. It is a test of an unsafe proposal, not a recovery feature.

The unchanged external verifier has an additional wall-clock expiry:
[`B3FinalityVerifier.sol`](../contracts/src/B3FinalityVerifier.sol) rejects
certificates after `MAX_EPOCH_LAG` and exposes no reinitialization/emergency
reset. Same-epoch certificates do not renew that timer. A B3-only rule cannot
promise to recover an expired deployed verifier. This is a source-level
constraint; no deployed contract state was queried for this work.

### Proposed next design boundary (not implemented or activated)

- Preserve the original incident and all original signing records as regression
  evidence. Never reuse a cleared/replaced journal to manufacture quorum.
- Specify a separately versioned finality-recovery transition: accepted proof
  or explicit trust authority, old-signature treatment, exact checkpoint/set
  commitments, state conservation, replay, restart and competing-transition
  rejection. Accepting a fully verified transition must be atomic; bypassing
  just `lineage_broken` is incomplete and unsafe.
- For future epochs, agree durably on the checkpoint/chain decision before
  issuing externally usable final attestations. Complete view-change and
  restart rules are required; the existing isolated agreement model is not
  automatically a production B3 finality implementation.
- Maintain a deliberate decision gate for unchanged-verifier compatibility
  and any mainnet activation. No live recovery anchor is selected here.

An explicitly trusted, disposable-regtest-only recovery checkpoint is a
possible **owner decision**, not inferred permission for an automatic unlock.
It cannot be advertised as trustless recovery or recovery of a deployed
external contract. Without that decision or an adequate existing proof, the
preserved network must remain paused. A fresh disposable network, if later
authorized, is also not recovery of the old network.
