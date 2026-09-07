# Finality lock at 814,191: incident assessment

Status: investigation and recovery proposal, not an approved recovery or a
claim that the network/bridge has recovered. Observations below were made on
2026-09-07 and must be refreshed before an operational decision.

## Evidence and scope

An operator reported a signer whose last vote and reported ancestry lock are
at checkpoint 814,191 on block
`c37424709c46b1da188d960cce8090b4f159097405a089dabecdf83d3f43eb25`.
Its public validator identity was redacted. The label "key-2" does not identify
validator index 2. The operator reportedly has two validator keys; neither
their identities nor both keys' recovery requirements are established.

The local node reports block 814,191 as
`0e673fee8445cb51879bb1a5cad8fc0d8d957791cd3a998fb239d3814f504759`
and included finality at 814,051. Its fullest retained checkpoint observation
at 814,741 had 21 distinct verified signers and weight 67,718,432, below both
the required 23 signers and weight 84,072,043. The newest checkpoint may have
fewer collected votes; these counts are per checkpoint and must never be
combined across heights. They do not establish that a missing validator is
offline or that no other node has additional valid votes.

`FinalitySigner::EnsurePersistentSafety` requires a strictly newer included
certificate from the exact epoch and signing set of an orphaned lock. Thus
814,051 cannot release the reported 814,191 lock. If the remaining eligible
signers cannot form quorum without the locked validators, this condition is
a circular liveness dependency, not a gossip problem.

The mainnet configuration permits a 1,440-block pre-finality reorganization
horizon while signatures become eligible at 12 blocks of depth. Twelve-block
burial therefore is not itself finality: an honest signer can acquire a lock
on a block that is later orphaned. These constants do not, alone, establish
which event caused this operator's fork, but show why delivery repairs cannot
rule out recurrence. Changing either value is not part of this update.

The existing mainnet one-time recovery matches epoch-0 incident 811,631 and
hardened anchor 811,641. It does not match this incident.

## What the ordinary wallet update can and cannot do

Bounded retention/retry of early messages, alternate-source retention,
verification-budget improvements, and explicit read-only signer diagnostics
can reduce missing-vote incidents and make actual lock failures visible.
They do not authorize an orphan-locked validator to vote on another branch.
No change to quorum, signing depth, signed messages, journal safety, recovery
pins, or bridge contracts is part of that transport/diagnostic update.

Restarting, waiting, rebinding a key, deleting or replacing a journal, or
broadcasting another validator's votes is not a proof releasing this lock.
Preserve each identity's original journal and wallet. Never run two signers
for the same validator identity or copy one identity's journal over another.

## Requirements before an incident-specific recovery decision

1. Establish the public identities and exact persisted vote/lock metadata for
   every affected signer, including epoch and both validator-set hashes.
   Use read-only diagnostics; do not request private keys or seed phrases.
2. Confirm the chosen chain independently with participating operators,
   reconcile the applicable validator snapshot, and check whether correct-chain
   signatures already retained elsewhere can satisfy both thresholds. One
   additional validator alone cannot turn 21 distinct signers into 23.
3. Read the actual deployed Ethereum verifier's accepted checkpoint/root,
   epoch, committed successor, deployment code, and time limits. Reconcile
   any known conflicting certificate, deposits, and withdrawals. Not seeing a
   certificate locally is not proof that nobody retained its signatures.
4. Obtain an explicit coordinated recovery ruling and bridge-security review.
   Do not infer network agreement from this node's active tip or from a request
   to repair message delivery.
5. If that review approves a pinned recovery, select an independently agreed
   checkpoint above every incident it covers, within each applicable signing
   epoch/window. Enforce the chosen history in block validation, match exact
   incident metadata, preserve recorded votes, and allow subsequent signing
   only above the approved anchor. Test wrong-domain/hash/set/journal rejection,
   restart persistence, and conflicting-chain rejection before deployment.

No new anchor or recovery exception has been selected or implemented here.
Distinct incidents must not be treated as one incident merely because both
operators report "finality signing disabled".

## Bridge safety boundary

The repository's `B3FinalityVerifier.submitCertificate` validates lineage,
monotone height, timing, and BLS quorum. It does not validate B3 block ancestry
or automatically learn a checkpoint added to wallet software. Whether the
live deployment matches this source must be verified separately.

An old signature remains cryptographically usable if it can be assembled
into a certificate satisfying the deployed verifier's remaining rules. A
one-wallet override or a B3-only checkpoint therefore does not, by itself,
establish bridge safety. Stopping our relayer does not stop permissionless
certificate submission. The contracts here do not have a general operator
pause/upgrade, and a replacement deployment does not automatically migrate
existing reserves. Do not encourage new bridge deposits while recovery safety
is unresolved.

## Preventing recurrence

A general solution needs a reviewed finality protocol with explicit voting
rounds, lock/unlock proofs and validator-set transitions, assessed together
with the bridge verifier. Fewer proposer forks and more reliable relay help
liveness, but are not a substitute for those protocol rules. Repeated ad-hoc
journal overrides are not a general recovery mechanism.
