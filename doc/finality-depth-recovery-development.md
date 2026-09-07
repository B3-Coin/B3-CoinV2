# Twenty-block signing policy and offline recovery prototype

Status: development work, not a release or an activated recovery procedure.
No live wallet, signer journal, peer configuration, or bridge account is
changed by this work.

## Production-source change

The mainnet staking loop uses a 20-block minimum age for new finality
signatures. The consensus minimum remains 12 blocks, checkpoint spacing
remains 10 blocks, and the certificate format, quorum thresholds and
validator-set rules remain unchanged. Other networks retain their scaled
consensus signing schedule. A larger consensus minimum cannot be weakened
by this local policy.

This is a delay before issuing new signatures, not a rule for rejecting
otherwise-valid peer signatures or blocks. Mixed-version nodes can therefore
continue to exchange ordinary V1 votes; older producers may still sign at
12 blocks. Eight checkpoint slots remain available for signature collection.
Finality still requires enough signatures AND stake weight for the same
checkpoint and signed object, collected before the accepted epoch window
closes. The delay adds eight blocks to the earliest signing time compared
with the mainnet consensus minimum. It is not a guarantee against deeper
forks.

During a mixed-version rollout, the newest checkpoint row in
`getfinalitystatus` can contain votes from 12-block signers before updated
wallets are willing to sign it. At depths 12 through 19, an updated wallet's
absence from that row is expected, not by itself evidence of failed relay.
The older eligible rows and the actual checkpoint age must also be checked.

The existing maximum signed height, durable ancestry lock, missing-journal
checks and certificate-based recovery conditions remain in force. A wallet
which has already signed ahead of its newly delayed target waits until a
newer checkpoint reaches the required age. It must not sign backwards or
rewrite its earlier vote to adopt the delay.

## User-proposed recovery rule

The proposed rule finds an earlier signed checkpoint still on the active
chain after the latest signed checkpoint is orphaned, and then considers a
new checkpoint at least 20 blocks deep and above the maximum-ever-signed
height. The offline prototype explores that selection only. It requires
explicit opt-in and explicit historical evidence, and produces a proposal,
not authorization to sign. It has no key access, journal writes, network
transport, wallet setting, or RPC integration.

The current durable signer format records the latest vote and ancestry lock,
not a sequence of all previous signed checkpoints. Existing wallets cannot
invent the missing history. A missing or deleted signer record is not an
empty history and is not eligible for this model's fallback.

## Known residual risk -- not a release gate passed by this prototype

Both the orphaned checkpoint and its replacement can descend from the same
earlier checkpoint. Keeping the maximum-ever-signed height prevents
conflicting signatures at one height, but does not prevent conflicting
histories at different heights. Waiting 20 blocks also does not revoke a
signature previously exposed through the peer network, a local aggregation
pool, or any certificate export.

The existing Ethereum verifier accepts a valid higher checkpoint without a
proof that it descends from the previously accepted B3 checkpoint. Therefore
the prototype includes a real-BLS, production-certificate-verifier
counterexample: increasing-height common-ancestor fallback can support two
incompatible quorum certificates. A passing counterexample test confirms
the residual risk; it does NOT certify the recovery algorithm as safe.

Consequently there is no automatic orphan-lock rollback in the production
wallet patch. Existing incidents still need the current valid recovery proof
or a separately reviewed and explicitly coordinated recovery boundary,
including the treatment of previously issued bridge authorizations.

## Qualification

Local macOS arm64 qualification, 2026-09-08:

- Rebuilt `test_bitcoin`, including the changed production signer and staking
  code. A test-only helper naming collision with the generic `Hash` function
  was corrected before the successful build.
- Passed 30 cases / 440 assertions in 1.32 seconds wall time: nine
  `finality_signing_policy_tests`, nine
  `finality_depth_recovery_prototype_tests`, five existing
  `finality_schedule_tests`, five existing `finality_certificate_tests`, and
  two existing `finality_signer_store_tests`.
- The delay tests exercise production `MaybeSign`, real BLS signatures,
  pool admission and temporary durable journals. Tests verify 19/20/21-block
  boundaries, upgrade waiting, 12-block peer-vote acceptance, a stronger
  consensus minimum, unchanged missing-journal guards, and continued refusal
  of an orphaned ancestry lock.
- The recovery counterexample passed: BOTH incompatible certificates verify
  using production BLS and certificate-placement checks on their respective
  synthetic branches. This confirms the known safety gap; it does not
  qualify fallback for deployment.
- Chain/tracker state is explicitly synthetic. These checks do not qualify
  full block execution, network rollout, Ethereum execution, Windows builds,
  or automatic recovery of existing locked validators. No long legacy-prefix
  mining suite was run; 1,405 unrelated cases were not selected.

Reproduce from the repository root after building:

```sh
build/bin-ui-preview/test_bitcoin \
  --run_test=finality_signing_policy_tests,finality_depth_recovery_prototype_tests,finality_schedule_tests,finality_certificate_tests,finality_signer_store_tests \
  --report_level=detailed --log_level=message
```

No Qt wallet was rebuilt, restarted or replaced for this change. No release,
tag, push, or mainnet recovery activation was performed.
