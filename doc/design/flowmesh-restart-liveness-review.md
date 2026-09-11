# FlowMesh restart and reconnect liveness repair

## Scope

This is a V1 wallet/runtime policy repair, not a B3 consensus change. The
proposal, attestation, certificate and checkpoint encodings, committee
membership, quorum threshold, execution rules and permanent signing store
are unchanged. Older wallets can still verify the resulting certificates;
older relay/signing nodes do not acquire these runtime fixes automatically.

## Reproduced failures

1. Local proposal rounds restart at zero and progress independently. The old
   current/current-plus-one receiver filter can permanently separate workers
   at the same certified sequence. An observer can also discard a proposal
   solely because its own timer is ahead or behind.
2. A silent legacy peer was probed only once. A peer temporarily reconciling
   its B3 index could discard that request and never be retried without a
   new connection, announcement, or authenticated future message.
3. A competing authenticated proposal caused a permanently locked receiver
   to halt even though it could safely keep retrying its original candidate.
4. A proposer broadcast its own attestation only at first signing. Later
   local proposal retries skipped that cached vote. A peer joining after the
   first broadcast could therefore receive proposals indefinitely without
   the vote needed to certify them. Suppression during B3 reconciliation can
   produce the same missed-first-send condition. This gap remained in
   `5e9cb403a823` and was reproduced against that revision.

## Changed behavior

- Authenticate the designated proposer against the envelope's signed round.
  For the current sequence, verify the full domain, market, anchored epoch
  and committee, anchor/transition, parent and state, evidence and execution
  without treating the receiver's timer as certificate validity.
- Keep the adjacent-round timer nudge for compatibility. A distant signed
  round, including UINT32_MAX, cannot set or exhaust the local timer.
- Before accepting a competing candidate, read and cross-check the durable
  retained body, hash lock and in-memory mirror. Corrupt/inconsistent own
  state still fails closed. An authenticated different remote proposal is
  ignored before insertion, timer changes or signing, preserving the exact
  lock. An inconsistent locally generated proposal remains a halt.
- Retry legacy peer discovery after a minimum 60-second sweep backoff.
  Preserve one request per second globally, bounded peer scanning, existing
  per-peer/per-market in-flight limits and independent certificate checks.
- Keep the legacy `proposals_rejected_round` RPC field for compatibility and
  expose `proposals_verified_different_round` and `proposals_conflicting_lock`.
  These are observations, not online-validator counts or finality proofs.
- When retrying a local proposal, rebroadcast its byte-identical cached local
  attestations. Preserve all proposal, anchor, transition, key and durable
  lock gates; do not sign a different vote. Rate-limit each candidate/seat to
  one replay per second, sharing the existing global and market committee
  budgets. Serve oldest attempts first so wallets with more owned seats than
  the per-market budget are not permanently skipped. Disarmed keys do not
  initiate these retries. Existing direct replies to remote proposals remain
  unchanged.

## Safety and availability limits

No saved vote is erased, rewound or changed. Conflicting-certificate handling
and the store's refusal to append a different entry against its permanent
lock remain unchanged. One vote is not quorum; a four-seat market still needs
three signatures for the same entry.

The proposal-admission tradeoff is explicit: an active seat can choose a
signed round assigned to itself and race a previously unlocked recipient's
first candidate sooner than under the old local-round filter. This broadens
an existing availability attack opportunity; it does not permit counterfeit
certificates or double-signing. V1's permanent single-candidate lock protocol
can still split honestly or maliciously into incompatible candidates. A 2+2
split in a 3-of-4 market cannot be recovered by this patch. General recovery
requires a separately designed agreement/lock-transition protocol.

Ignoring remote competing proposals supersedes the proposal-triggered halt
expectations in the earlier committee-forwarding/evidence-retry reviews.
It does not supersede their certificate or journal integrity requirements.

Networking remains a deployment requirement. This patch cannot open a remote
firewall or make old intermediate nodes forward messages they do not relay.
Validators need outbound routes to updated FlowMesh-capable peers; an inbound
listening port is not required on every validator. Ordinary B3 peer count and
`running: true` alone do not prove a functioning FN quorum.

## Focused verification

The added large-round-skew and delayed-peer-retry cases fail against the old
runtime before the patch. Coverage includes three real BLS signer runtimes
connected through a keyless observer at differing rounds, adversarial signed
rounds and malformed/context/anchor/state proposals, preserved incompatible
locks, and a real service restart that ignores a competing proposal then
certifies its exact original retained candidate.

The round/reconnect patch's local verification passed 41 cases and 104,255 assertions: 22 runtime
cases (102,118 assertions), five service-status cases (63), two real startup
cases (1,414), and 12 production-store cases (660). The round-skew regression
also certifies a non-genesis deposit through the observer and verifies the
exact 250-unit ledger credit, parent hash, three-seat certificate and retained
signer locks. The test and Qt application targets compile locally.

The follow-up cached-vote regression starts a proposer before its recipients
connect. A late keyless observer must receive the exact original vote after
the retry delay, without extra same-clock/early copies. Two more signers then
join a line topology in which only the observer can assemble quorum; all four
nodes must converge on the same independently verified certificate and retain
the original lock. A second regression checks ten owned keys against an
eight-message retry budget, identical replay payloads, fairness and disarming.

With the cached-vote repair, all 43 focused cases passed (104,524 assertions):
24 runtime, five service-status, two real startup and 12 production-store
cases. Both Qt and the test program compiled locally. The late-observer case
failed at its required replay assertion against `5e9cb403a823` before passing
with the repair.

These are local deterministic tests, not evidence of WAN trading latency or
proof that the live market has resumed. Mainnet resumption must be confirmed
by an advancing certified microblock head and matching balances/fills.
