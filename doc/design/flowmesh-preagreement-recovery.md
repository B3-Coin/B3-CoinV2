# Fresh-market conflict recovery: isolated pre-agreement work

Status: **experimental runtime integration, not deployed or release-qualified**.
The source is being published as the explicitly experimental
`1.1.5-flowmesh-test.2` candidate. See [the tester guide](../../FLOWMESH-TESTING.md)
for retained failures and connection instructions; compilation is not deployment
qualification.
The isolated node build now includes a durable preliminary agreement engine,
wire codec and explicitly opted-in fresh-market path. Default markets continue
using V1. This does not repair any deployed market or authorize a signing-history
reset. The preceding proof-only report remains preserved separately as earlier
evidence; its passing tests did not demonstrate runtime recovery.

## Demonstrated defect

The V1 proposal path permanently locks `(epoch, sequence)` before publishing a
proposal. An attesting replica also permanently locks before signing. Two honest
proposers can select different, individually valid B3 anchors for the same
action. Their production-entry hashes differ even if execution and all resulting
balances are identical.

With nine seats and quorum seven, three votes for each of two incompatible
entries leave neither entry capable of reaching quorum while respecting those
locks. The remaining three seats can bring either side to only six. Restart,
reconnect, additional rounds, or creating another asset do not fix that rule.

`baseline_same_deposit_adjacent_anchors_split_nine_seat_quorum` exercises the
actual runtime, real BLS signatures, delivery and durable production stores in a
generated fixture. Its chain dependency supplies both canonical/deep anchors;
it is not a full B3 block-parent/reorg test. Its expected result is the existing
stall, not a repaired runtime. A passing test records reproduction of the defect.

## No B3 hardfork boundary

An operator-only preliminary agreement protocol can select one entry *before*
any permanent V1 signature. Its decided entry can use the existing final
certificate and settlement machinery. The following must remain unchanged:

- Production-entry identity/encoding and the `B3/FLOWMESH/CERT/V1` digest.
- The complete anchored FN roster, seat weights and `floor(2*n/3)+1` quorum.
- B3 anchor validity/depth, genesis, checkpoint continuity and epoch handoff.
- Type-8 checkpoint and type-9 withdrawal bytes, proofs and nullifiers.
- Execution, action bytes, balances, fees, market identities and settlement.
- Existing permanent signing locks, persist-before-sign and publication.

This is still an **operator protocol upgrade**, not a config-only repair. B3's
unchanged rules do not authenticate the choice of preliminary protocol. All
participating signers must use the same new mode for an explicitly eligible
fresh market. An old V1 operator must not silently continue early final signing.
Compatibility of eventual emitted transactions must be tested through actual
old-rule block validation before deployment; unchanged source alone is not that
qualification.

## Preliminary protocol under evaluation

Protocol reference: Castro and Liskov's [PBFT algorithm, normal operation and
view changes, sections 4.2 and 4.4](https://www.usenix.org/legacy/publications/library/proceedings/osdi99/full_papers/castro/castro_html/node4.html).
This is a bounded PBFT-style integration, not a correctness proof of the complete
algorithm or a claim of unbounded partial-synchrony liveness.

Use a single-slot PBFT-style agreement, not a timeout-based unlock:

1. Independently validate an exact candidate and its action/evidence bytes.
2. Publish domain-separated PREPARE votes under a justified view.
3. A quorum of those votes proves preparation, **not a final decision**.
4. Publish separate COMMIT votes only after validating that prepared proof.
5. A quorum of COMMIT votes proves a decision. Persist its complete proof and
   candidate before permitting the existing permanent V1 signing path.

A later view requires a complete quorum of signed view-change reports. Each
report commits to its highest prepared proof, if any. The selected candidate
must match the highest prepared proof in that quorum; conflicting candidates
at any same view among those proofs are rejected. A proposal's claimed higher view, a
single timeout, or a single peer's claim is not authorization to replace a vote.

Domain separation must bind protocol version, phase, domain, market, epoch,
anchored set, slot sequence, parent, prior state, execution configuration, view
and candidate. A preliminary signature must never verify as a V1 final share.
The experimental verifier checks proof objects only. It does not enforce an
honest signer's local history, discover the globally highest proof, guarantee
execution validity or establish network liveness. Those require the state
machine and durable integration below.

## Integration requirements and qualification checklist

### Durable state and downgrade protection

- Persist exact signed preliminary objects and candidate/evidence before
  publication; sign at most once per seat/phase/view/slot.
- Persist monotonic view, highest prepared proof and final decision atomically
  with the associated signing record. Reopen and validate them before arming.
- Once view-change to view `v` is durably published, never create a new PREPARE
  or COMMIT for an older view. Exact retransmission of an already durable vote
  is different. Test delayed old-view prepared proofs on both sides of a
  restart: they must not resurrect old-view signing after a nil view-change.
- Retain decisions until final V1 certification and normal history advancement.
  Do not forget a decision because its B3 anchor becomes invalid.
- A failed or uncertain storage write is a safety stop, never permission to
  retry a different value. Add interruption/failure tests at each boundary.
- An unknown extra record in a V3 production store is insufficient: the old
  reader ignores unknown namespaces. A new-mode store needs an incompatible
  local marker at the **same** market path so downgrade refuses to sign.
- Empty local storage is not proof that a market has no remote history. Require
  explicit eligibility for a genuinely fresh market and reject old histories.

### Runtime and transport

- Do not use `SignProductionProposal` for preliminary proposals: it already
  creates the permanent lock being avoided.
- Gate **every** local legacy proposal/attestation path, including retry,
  catch-up, restart and externally delivered V1 messages, on a durable decision
  when the market is in the new mode. Never fall back to legacy signing.
- Authenticate scheduled leaders and justified new views; define monotonic
  round advancement and timeout/view-change publication completely. Arbitrary
  higher peer rounds must not move local state.
- Reuse bounded transport admission/retry machinery while retaining exact
  objects. Decide and test proof/candidate fetching, missing evidence,
  reconciliation pause/resume and stale traffic cleanup. No signing-state reset.
- Add bounded, distinct preliminary wire codecs and resource limits. The proof
  verifier's local object budget is not a consensus seat cap or a network codec.

### Required qualification

- Competing honest candidates, healed partitions and leader equivocation.
- Hidden prepared/commit quorums, stale and cross-context/phase messages.
- Two faulty/offline seats of nine versus three missing seats; retain quorum.
- Restart/crash at every persist/sign/publish boundary; exact-byte retry.
- Downgrade/mixed-mode/legacy-message refusal with no V1 signature bypass.
- Real independent operator delivery with advancing B3 anchors and actual
  unchanged checkpoint/withdrawal block acceptance on legacy B3 validation.

Proof-verifier unit tests cannot substitute for these integration checks.

The experimental verifier caps a complete proof at 4,096 signatures
before cryptographic work. This deliberately bounded prototype does **not**
support every proof at the existing maximum committee size: a two-quorum
view-zero commit fits through 3,071 seats, and the worst-case later-view proof
with a prepared certificate in every report fits through 92 seats. The opt-in
agreement mode refuses an unsupported full roster; it never truncates it or
reduces quorum. Legacy mode and B3 consensus limits are unchanged. Full resource
and operator-network qualification remain outstanding.

## Runtime integration demonstrated so far

- Explicit repeated `-flowmeshpreagreementmarket=<Asset-derived MarketId>` opts
  in only a fresh market; no automatic conversion of existing namespaces.
- Format-v4 marker at the same per-market path refuses a v3 reader, mode flips
  and a missing agreement journal after completed bootstrap. A two-store
  identity handshake completes before any signing. A valid connected B3
  checkpoint also prevents fresh-mode activation.
- Exact preliminary signing intents, signed messages, execution evidence,
  highest prepared proof, view and decision are synchronously journaled.
- Local V1 proposal signing is disabled in new mode. Final V1 attestation is
  permitted only for the exact durably decided candidate after fresh execution
  and anchor checks. Valid received V1 history still supports ordinary catch-up.
- Separate preliminary FMN2 messages are bounded and authenticated, with full
  quorum proofs rather than trust in a peer's claimed round. Admission is not
  peer receipt or certification. Transient delivery failures retain retries.
- Generated actual-runtime tests demonstrate genesis/deposit certification,
  no V1 locks before commit decision, exact reopen and missing-journal refusal,
  and a healed four-runtime 2+2 partition with divergent preliminary views.
  These use an in-memory transport and a mock chain dependency, not four
  deployed processes or WAN evidence.
- Focused engine tests cover a nine-seat preliminary split and seven-versus-six
  availability, hidden prepared proofs, stale views, exact signing/decision crash
  boundaries and restart. These do not themselves qualify the full runtime
  through the exact nine-seat same-action/adjacent-anchor incident.

The subsequent nine-runtime same-deposit/adjacent-anchor split/heal test passed
using real BLS and durable stores, but still with a mocked chain and in-memory
transport. Two actual four-operator/fifth-client independent-TCP runs passed
genesis, deposits, orders, matches, cancellations and type-8 constructed-block
engine-on/off parity, then both failed a later remote withdrawal's certification
within 90 seconds. All four evaluated the same candidate; rate-limit deferrals
and critical queue refusals were observed before a complete COMMIT certificate.
This repeatable delivery/progress failure remains open. The later bulk/rejoin
phase and remote type-9 constructed-block parity were not reached. Passing base
custody/payout tests do not substitute for those missing checks.

Remaining qualifications include end-to-end withdrawal after resolving that
stall, independent bulk/fault/rejoin delivery, historical-binary compatibility,
and unchanged withdrawal constructed-block acceptance. Explicit resource limits can halt safely (including
128 preliminary views per slot); this is not a guarantee that every partition,
anchor invalidation or indefinitely missing quorum can recover. Operators need a
coordinated upgrade: an older FMN2 parser rejects the new kind. Inner production
formats and B3 validity remain unchanged, but mixed-version operator operation
is not qualified. Transport is still authenticated plaintext TCP, not encrypted.

## 2026-09-19 delivery-stall repair (uncommitted, pending independent audit)

Correlated Run04 traces isolate the first blocked step. Slot 12's view-zero
proposal was socket-written 06:21:56.696Z but first read by its peers at
06:21:58.8-59.2Z, after every two-second round had expired. Each receiver's
per-peer committee admission (burst 32, 8 messages/second) was being spent by
exact duplicates of slot 11, which was already certified: 13 distinct messages,
131 publications, 393 per-peer deliveries. A refused frame holds the FMN2
critical channel, so the new proposal waited behind them. Votes for view zero
were therefore never generated by two of the four seats, and later views' reports and
votes were generated but delivered after their own rounds (12,615 publications
of 176 distinct messages in the failed slot). No limit was the cause.

Two coupled runtime changes, no protocol, journal, quorum or limit change:

- A verified message is forwarded at once, never back to its supplier; an
  exact repeat is re-forwarded at a doubling interval (1 s to 32 s) instead of
  by every receiver every second. The signer's own durable retry is unchanged.
- Once the agreement engine has verified and retained a payload, identical
  bytes are consumed at runtime admission until that recovery forward is due.
  They spend no committee admission and cannot hold the channel. Different
  bytes, including a COMMIT repeated with newly attached proof, never match.

`preagreement_paced_slots_survive_fmn2_committee_admission` reproduces the
stall on the previous source with unchanged production limits and passes after.
Two further four-operator/fifth-client runs certified the remote withdrawal in
0.64-0.67 s, completed its type-9 payout and passed type-9 constructed-block
parity. Neither run completed the bulk/rejoin phase: one met a `bad-anchor`
STORE_FAILURE halt in the shared V1 commit path as a B3 block connected during
certificate append, the other a `seat_transition_paused` gate behind a pending
B3 checkpoint. Both are outside agreement delivery and remain open. A paused
market with pending work also keeps consuming its 128 preliminary views.

## Resilience integration follow-up

The delivery-stall report above is retained historical evidence, not a claim
that its entire fault campaign passed. The follow-up adds these bounded local
repairs without changing the signed formats, B3 validation, quorum or limits:

- Duplicate suppression compares the complete validated envelope as well as
  payload identity. Certified retired-slot duplicates stay quiet within the
  existing bounded FIFO. Distinct proof attachments still require validation.
- Authenticated old-view candidate bodies are retried from their exact retained
  bytes after missing evidence arrives. Bounded fair recovery cannot outrank a
  pending full decision or revive old-view voting.
- Append APIs identify validation refusals that occur before any database
  write. Only a canonical-anchor refusal overlapping B3 reconciliation is
  deferred; invalidated anchors and actual storage failures still halt.
  Catch-up pages retain the exact interrupted certificate and a bounded
  obligation to fetch the remainder, rather than relying on unsolicited gossip.
- Local timeout accounting begins with eligible work and suspends across
  observed pause/key/chain gates. Resumption preserves the remaining budget so
  repeated short pauses cannot starve a legitimate missing-leader view change.
  Durable view history, the 128-view cap and the configured timeout are unchanged.
- Chain pause diagnostics distinguish reconciliation, genesis/settlement
  checkpoint requirements, unavailable settlement/seat state and handoff
  backlog. Required barriers are reported even with an empty action pool;
  identical observations are coalesced rather than logged each refresh.

The process fixture must complete the existing withdrawal retirement path:
type-9 payout reaches the canonical 30-deep anchor, the operators certify its
settlement entry, then an ordinary type-8 transaction anchors that entry.
Earlier effect-bearing checkpoint entries are drained in order. Only after
the final checkpoint is recognized must the fixture require all replicas to
be unpaused. Skipping this publisher step is not a transport-failure test;
bypassing the pause is not a valid repair. This integration does not introduce
an automatic fee-spending checkpoint publisher.

Focused controls compare independently built prior production code with the
same new tests. The qualification record must distinguish those unit results,
the full process campaign, and any fixture-only failures. A consumed receipt
must be rejected explicitly as already nullified; an arbitrary RPC error does
not qualify one-time consumption. None of these tests qualifies WAN latency,
power-loss durability, mixed-version operation or recovery of old final locks.

## Existing markets and remaining failure boundaries

Existing fUSD/dUSD split final signatures remain protected. Do not delete or
rewind their journals, force signing, or reuse their balances in another market.
The V1 market identity has one market per base AssetId; the fresh-market route
therefore needs a genuinely unused base asset or a separately authorized future
market-identity extension. No new asset or market is created by this patch.

Recovery aims to handle **undecided preliminary conflicts**. It does not promise
recovery if a prepared candidate's B3 anchor becomes invalid: a view-change
proof may still require that candidate even before a commit decision. Nor does
it promise recovery after invalidation of an already decided/final-signed anchor, or
progress without quorum/eventual delivery. Those cases remain explicit safety
halts until a separately validated protocol path exists.

Publication and desktop compilation were subsequently authorized for testing.
No production deployment, key changes or live economic actions are part of this
candidate. Preserve prior failed-run evidence and exact signed histories.
