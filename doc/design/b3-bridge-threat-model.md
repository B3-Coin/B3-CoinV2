# B3 FlowMesh bridge — threat model (deposit leg v1, release leg as specified)

Written 2026-08-25, the day after the first real mainnet deposit was proven
(vault `0x143F207e23e6aebD7E974be90ac6D434f4c7BFb6`, deposit id 0). Purpose:
an honest adversarial inventory for the owner. Nothing here is marketing;
severity is judged for the FUTURE state where real value flows, not the
smoke test. Companion documents: b3-bridge-bls-proposal.md §5 (original
threat notes), b3-cross-chain-finality-v1.md (release-leg protocol),
b3-product-identity.md.

## 0. Trust model, stated exactly

A deposit mints on B3 iff ALL of:
1. one bootstrap checkpoint (a human/release trust decision, made once),
2. a supermajority (>= 342/512 default) of Ethereum's sync committee signed
   the finalized header, with every committee handover proven back to (1),
3. keccak/SHA-256 links from that header to the deposit event bytes
   (finality branch, execution branch, ancestry chain, receipts MPT), and
4. (stage 4) B3 consensus accepts the proof and applies mint rules.

A withdrawal releases on Ethereum iff a quorum (floor(2W/3)+1 by stake
weight) of B3's Modern-PoS validator set signed the finality certificate,
with set handovers proven back to the genesis set pinned in the verifier
contract, and the burn proves into the withdrawal tree.

There is NO custodian, multisig, oracle, or upgrade admin in either leg.
Consequently every threat below is either (a) an attack on one of the two
consensus systems, (b) an attack on the two pinned trust roots, or (c) a
defect in our code. There is no "operator risk" category — by design.

## 1. Deposit leg (ETH -> B3)

### T1. Corrupt sync-committee supermajority — CRITICAL, expensive
342 of 512 colluding committee members can sign a fake "finalized" header
containing a fake receipts root -> arbitrary fake deposits -> unlimited
B3-side mint. Committee seats are sampled from ~1M validators, so a
supermajority of seats requires roughly a supermajority of all staked ETH
(tens of billions of dollars) or a catastrophic randomness failure; signing
a conflicting finalized checkpoint is also slashable equivocation.
Mitigations in code: finalized-only (never attestation-weight headers),
supermajority threshold (342, config-pinned at A3).
Mitigations still REQUIRED at stage 4 (owner rulings): per-block/per-epoch
mint caps so even a total committee compromise has bounded damage before
humans react; optional watcher veto delay. **Do not ship minting without a
cap.**

### T2. Bootstrap checkpoint poisoning — CRITICAL, cheap if process is weak
Everything chains from the pinned checkpoint root. A wrong/poisoned pin in
a release = attacker-controlled Ethereum view for every B3 node. This is a
process threat, not a code threat.
Required rule: the A3 checkpoint is pinned in a reviewed release commit,
cross-checked against multiple independent beacon sources; the same "four
release gates" discipline as H/X pinning.

### T3. Long-range / stale-client attack — HIGH
Validators who have since exited (nothing left at stake) still hold keys
that once formed committee supermajorities. A client that has been offline
longer than the weak-subjectivity window can be fed a fabricated committee
succession built from those dead keys.
Required rule (per proposal §5): a maximum sync-lag; a node whose light-
client state is older than the bound must fail closed (deposits halt) and
re-bootstrap through the checkpoint process. Halting is safe; catching up
through a fabricated history is not. Not yet implemented — stage 4 work.

### T4. Ethereum-side deep failure — MEDIUM, external
If Ethereum's finality itself fails (mass slashing, inactivity leak,
or a social-consensus rollback of finalized history), B3's view and
Ethereum's canonical chain can diverge AFTER mints occurred: minted B3
assets against reserves that no longer exist. No protocol fully survives
its origin chain's finality reneging. Mint caps (T1) bound the damage;
beyond that this is accepted origin-chain risk and should be stated in any
user-facing documentation.

### T5. Fork-upgrade freeze or misverification — HIGH likelihood, low harm
if handled; HIGH harm if not
Ethereum hard forks change fork_version (breaks the signing domain) and
occasionally the state layout (Electra moved the light-client gindices —
already handled by epoch switch). A future fork can freeze the client
(deposits halt: safe) or, worse, a naive constant reuse could misverify.
Required rule: fork schedule + gindices are B3 consensus constants updated
by release BEFORE the fork activates; unknown fork_version => fail closed.
The code already selects gindices by epoch; the "unknown version" refusal
is stage-4 wiring.

### T6. Relayer censorship / liveness — LOW (delay only)
Relaying is permissionless; anyone can submit updates and proofs. A
censoring relayer delays deposits, never forges them. One update per
committee period must reach B3 or catch-up needs archived updates (beacon
nodes serve historical periods). Denial = delay, not loss.

### T7. Vault contract defects — HIGH until audited
Current state: 16/16 foundry tests; reentrancy guard added after the
author's own review found a real hook-token inflation vector (fixed,
attack-tested); balance-delta accounting against fee-on-transfer
over-mint; surplus-only rescue (cannot touch `locked`); no owner, no
upgrade, immutable authorities; USDT's non-standard returns handled.
Residual: single-author review only. **A third-party audit is a hard gate
before non-trivial TVL** (belongs on the A3 gate list). Known accepted
properties: authority key loss strands funds (production authority = the
verifier contract); issuer blacklisting (USDT can freeze the vault) makes
the B3 asset trade against a frozen reserve — OD-8 policy question, open.

### T8. Malicious/weird tokens — CONTAINED by design
Anyone can deposit any token and emit true-but-worthless events; the vault
cannot police token honesty (a token controls its own balanceOf). The B3
asset registry (stage 4) is the firewall: only registered, vetted,
hook-free assets ever mint. Rebasing/deflationary tokens must simply never
be registered. Registration is an owner-ruled consensus act.

### T9. Proof-layer parsing defects — MEDIUM, in OUR code
The strict decoders (canonical RLP, MPT inclusion, SSZ, receipt/envelope,
ancestry) are exactly where forged-bytes bugs would live. Current
assurance: strictness-first implementations, negative tests for every
malformation class we conceived, real-mainnet anchoring (fixtures refuse
to emit unless independently rebuilt roots match Ethereum consensus).
Required before consensus wiring: fuzz harnesses over RlpDecode /
VerifyMptProof / DecodeReceipt / VerifyExecAncestry / ssz branch
verification, and an external audit of the verification headers.

### T10. Double-mint / replay — MUST-DESIGN at stage 4
Nothing currently prevents proving the same deposit twice because nothing
mints. Stage-4 consensus MUST carry a per-origin nullifier set keyed by
(origin_chain, deposit_id) with reindex/undo correctness. Also: exact
`b3Recipient` semantics must be normative before any mint — ambiguity
there is mint-stealing.

### T11. Resource exhaustion — LOW
Proof sizes are naturally bounded (MPT log-depth; ancestry chains bounded
by the sync-lag rule of T3; committees fixed 512). Stage-4 verification
costs must be priced through the existing MPA verify-cost budget so a
block full of proofs cannot stall validation.

## 2. Release leg (B3 -> ETH) — specified, not yet built

### T12. Small early validator set — THE key activation threat
MIN_FINALITY_SET = 4 is a chain-bootstrap floor, NOT bridge security. If
the Ethereum verifier is activated while B3's staked set is small/cheap,
buying 2/3 of B3 weight is cheap and the vault's ENTIRE reserve is the
prize (sign a fake certificate + withdrawal root, drain via §6 proofs).
**Bridge activation (A3) must be gated on an explicit economic-security
threshold of the B3 set — an owner ruling, already flagged in the spec's
§9 and OD-8. This is the largest open design decision in the system.**

### T13. Verifier-contract correctness — HIGH until audited
B3FinalityVerifier + BlsCertificateProver (EIP-2537 pairing path,
members_root multiproofs, bitmap complement checks, epoch monotonicity,
withdrawal-tree released[] map) must byte-match the C++ side. The §8
shared test vectors exist as a requirement precisely for this; generate
them from the C++ implementation and run both sides in CI. External audit
before mainnet deployment.

### T14. B3 finality bugs feeding the bridge — bounded by fail-closed
The epoch machinery (handover-gated rotation, extension, lineage breaks)
already fails closed on B3. The verifier mirrors this: no certificate, no
release. Stuck > MAX_EPOCH_LAG => withdrawals freeze until governance;
freezing is the safe direction.

### T15. Validator BLS key theft — contained per-validator
Keys live in the wallet's encrypted opaque records (audited path, no
export/spend surface). Stealing one validator's key yields one weight
contribution, not the bridge; quorum still rules. Slashing for
double-signing is a Modern PoS design question (OD), not bridge-specific.

## 3. Cross-cutting

- **Single-author code**: every line of the bridge stack was written by one
  author (the assistant). The compensations — real-mainnet anchoring,
  strictness-first, negative tests, live E2E — reduce but do not replace
  independent adversarial review. Audits (T7, T9, T13) are the gates.
- **Dependency determinism**: blst pinned v0.3.17 in-tree; keccak/sha
  in-tree; no network code in consensus paths. Frozen-vector CI across
  platforms already part of release qualification.
- **Unit/denomination confusion**: amounts cross three systems (wei,
  token decimals, B3 kB3 1e9). Stage-4 mint math needs explicit,
  vector-tested scaling rules per registered asset. The known RPC
  parse/display quirk (1e8 vs 1e6) must NOT leak into bridge math.
- **Monitoring**: even with perfect code, run watchers comparing vault
  reserves vs B3-minted supply per asset; alert on divergence. Cheap, and
  it converts several CRITICALs into detected incidents.

## 4. Ranked gates before real value flows

1. Owner ruling: bridge activation threshold for the B3 validator set (T12).
2. Stage-4 design: nullifiers, asset registry, mint caps, b3Recipient
   semantics, sync-lag + re-bootstrap + unknown-fork fail-closed rules
   (T1, T3, T5, T8, T10).
3. Third-party audits: vault contract, C++ verification headers, Solidity
   verifier when built (T7, T9, T13).
4. Fuzzing of every byte-level decoder (T9).
5. §8 shared vectors generated and run against both implementations (T13).
6. Checkpoint-pinning release process, same rigor as H/X (T2).
7. Reserve to supply watcher tooling (§3 Monitoring).

The smoke-test deployment (owner-controlled authorities, 0.001 ETH) is
exempt from these gates by proportionality; nothing else is.
