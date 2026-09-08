# v1.1.5 preparation and branch policy

## Branches

- `master` is the source branch for reviewed development commits.
- `release/v1.1.5` follows the same reviewed commits during preparation.
- Fast-forward the release branch after each reviewed master update and push
  both refs together, atomically when the remote supports it.
- Do not force-push, overwrite another contributor's work, or silently reset
  a divergent branch. Inspect and reconcile divergence before synchronization.
- Working-tree prototypes are not synchronized by committing everything.
  Stage only reviewed files. Both branch tips matching does not imply that
  unrelated local uncommitted work was published.
- This is a development workflow, not a background synchronization service.
  No release tag, published binary, or CI release dispatch is authorized by
  branch synchronization alone.

## Validator inactivity: proposal, not active consensus

V1 membership is derived from eligible stake and non-revoked BLS bindings.
It has no missed-vote timeout. Current and successor sets are snapshotted in
advance, and the old authority certifies the successor. Ethereum can accept a
smaller properly authorized future set; it does not accept a relayer-chosen
replacement for a committed set.

The intended design work is future-snapshot eligibility based on agreed
participation rules, not deleting current voters to manufacture quorum.
Before implementing or selecting an activation height, resolve:

1. What chain-verifiable evidence counts as participation? A particular
   node's received gossip or peer connections cannot define global activity.
   Evidence design must account for censorship and delayed signatures.
2. How long is the observation window, and how are new validators and delayed
   epoch handovers treated? No arbitrary timeout is selected by this plan.
3. At which not-yet-committed snapshot does exclusion take effect, and how
   does a validator requalify? Consider the existing delayed set pipeline.
4. What minimum committee size, stake threshold and exclusion-rate bounds
   apply? Headcount and stake quorum protections must remain explicit.
5. What coordinated activation and old-wallet migration are required?
   Changing the consensus-derived validator set is not merely a local wallet
   policy change and cannot promise compatibility with unchanged V1 nodes.
6. How is a sustained quorum outage handled? Future eviction alone does not
   authorize a handover when the currently required quorum is unavailable.

Review protocol safety and liveness, adversarial partitions/censorship, B3
snapshot determinism, Ethereum successor commitments, and re-entry before
calling the proposal release-ready. This preparation approves no automatic
shrinking quorum, inactivity slashing, privileged removal or signer rollback.

## Other pending work

The existing uncommitted finality-recovery/RPC/Qt prototype remains separate
until its durable receipt installation, failure behavior and supported
platforms are reviewed and tested. It must not be described as shipped merely
because `release/v1.1.5` exists.
