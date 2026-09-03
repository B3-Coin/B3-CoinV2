# B3 Ethereum bridge — threat model (decentralized transition candidate)

**2026-09-02 decentralized pre-M amendment (current):** the intended release
now pins a new immutable staker vault and delayed-initialization verifier before
canonical Set_0 exists. Four already-bound BLS identities form an equal-weight
one-time bootstrap set; any three may attest the exact block-811,000 hash and
Set_0 header before an immutable deadline. Successful initialization removes
that path forever and hands authority to ordinary B3 stake-finality set
rotation. The old managed smoke vault remains historical and must not share the
new asset identity.

The new vault may be deployed before M, but it does not accept USDT deposits
before verifier initialization and a fresh valid certificate proving qualified
current and successor validator sets. Inbound bridge activation `B` is selected
only in a later B3 build that pins the complete audited deployment tuple; it is
not FlowMesh A3 and is not inferred as M. Missing or mismatching pins fail
closed. There is no corridor-deposit or pre-readiness custody phase.
Irreversible B3 burns use a second consensus height `W >= B`, which remains
unset until round-trip canonicality and liveness safety has actually been
solved, audited, rehearsed, and explicitly enabled. If B is enabled first,
depositors receive bUSD but knowingly wait for W before Ethereum redemption.

**2026-09-01 managed-vault ruling (superseded before activation):** the published
smoke vault was briefly considered for transition-v1. The current release instead
deploys the immutable staker verifier and a new keyless vault before any bridge
activation. This is a pre-activation replacement, not a user-balance migration:
the smoke vault held zero USDT at the recorded observation and no consensus bUSD
could be minted before B. Those zero-state facts must be rechecked, and the old
vault/AssetId must be excluded from the production registry. If either old-vault
liabilities or consensus bUSD ever exist before replacement, the simple
replacement assumption is invalid and the full migration procedure in §6 applies.

The transition tree now contains the B3-side stage-4 path: a bounded canonical
type-10 codec for bootstrap, update, mint, execution backfill, and both the
historical managed and current decentralized withdrawal forms; consensus
light-client/receipt/log validation; exact OWNER mint;
deposit nullifiers and block/epoch caps; exact bUSD BURN requests; reversible
connect/undo state; deterministic restart/reindex replay; and
mempool/miner/asset wiring. Each record requires exactly one zero-value policy-9
`BRIDGE_RECORD` metadata output committing the
`B3/BRIDGE/RECORD/V1` tagged hash of its exact canonical frame. Missing,
duplicate, mismatched, and orphan cells are invalid. For the retained
historical managed-withdrawal form, consensus permits only exact ECDSA
`SIGHASH_ALL` or Schnorr
`SIGHASH_DEFAULT`/`SIGHASH_ALL`; `NONE`, `SINGLE`, and `ANYONECANPAY` are
invalid. These signatures bind the bridge fields through the normal output
serialization, including the Ethereum recipient; no `OP_RETURN` or custom
bridge sighash is used. Candidate checks use small
base-plus-overlay state rather than copying lifetime bridge history, and recent
undo is bounded to 288 blocks; a deeper reorganization deliberately marks the
index dirty and rebuilds it from canonical history. The bridge index is in
memory and rebuilt from activation. No durable sidecar exists, so a configured
bridge refuses pruning and any AssumeUTXO snapshot that skips bridge history.

Mainnet nevertheless remains fail-closed. Adapter commitments are not yet
enforced against Ethereum token implementation/code state; production
checkpoint, fork, cap, approval, activation, rules, and X-dependent pins remain
unset; runtime reproducibility and audit evidence remain open; and the exact
deployer/bootstrap manifest and chainparams match are not yet approved. The
permissionless certificate/withdrawal RPC and calldata tooling removes the old
operator-key requirement, but it does not close any of those security gates.
Filling parameter fields must not be described as satisfying them.

The historical smoke-vault facts were checked through two independent Ethereum RPC providers at
block 25,877,643: release and rescue authority are the same EOA,
`0x76c7a245d0D2e4CF92403aF0144825df1cC614f1`; the vault runtime code hash is
`0x1be220c18efa4e4cda0bb1c912c7c41346f5c04d49a36ec2c68f6ddcc5586233`.
The vault held no USDT then. These facts close identity inspection only; they
do not close proof, cap, audit, operating-rule, or reserve-funding gates.

### Historical managed-withdrawal workflow (not current activation)

1. A B3 request provably burns canonical bUSD and binds its exact raw
   six-decimal amount and Ethereum recipient. Consensus identifies it uniquely
   by the stable base transaction id plus burn-output index. Because the signed
   policy-9 output commits the complete type-10 record, witness re-signing
   cannot create a second request identity.
2. The operator waits the pinned B3 finality depth and verifies that the burn
   remains canonical. A pending, replaceable, or reverted burn authorizes
   nothing.
3. The managed authority calls the published vault's `release` exactly once
   for the registry-derived USDT token, bound recipient, and exact amount, and
   durably records the consumed request id before any retry.
4. Operations continuously reconcile canonical bUSD supply and burns against
   vault reserves and releases. A mismatch halts new mints and releases.

This workflow remains the minimum safety record for the historical managed
contract, but that contract is not the current production target. The current
keyless vault verifies the staker-finalized withdrawal root and prevents repeated
withdrawal IDs in Solidity.

### Deposit event and proof binding (implemented consensus path)

The successful Ethereum receipt must be proven from the pinned sync-committee
checkpoint through finalized header/committee updates, the execution-payload
branch, a full parent-hash ancestry chain when the deposit block is older, and
a receipt-trie inclusion proof at canonical `RLP(transaction_index)`. The mint
selects one exact log index. If a carrier redundantly includes both transaction
index and trie key, consensus requires `key == RLP(index)`.

The selected log emitter is the exact approved vault and has exactly three
topics: `keccak256("Deposit(uint64,address,uint256,bytes32)")` =
`0xdaf0af297d25c0e96a0b209d35692b4e07c503634eeca57fc5c35c006acf527f`,
the 24-zero-byte-padded `uint64 depositId`, and the 12-zero-byte-padded token
address. Data is exactly 64 bytes: big-endian `uint256 amount` followed by
`bytes32 b3Recipient`. For canonical bUSD, token is the pinned USDT address and
the received raw amount converts exactly six decimals to six decimals.

Written 2026-08-25, the day after the first real mainnet deposit was proven
(vault `0x143F207e23e6aebD7E974be90ac6D434f4c7BFb6`, deposit id 0). Purpose:
an honest adversarial inventory for the owner. Nothing here is marketing;
severity is judged for the current decentralized candidate where real value
could flow, not the historical smoke test. Companion documents:
b3-bridge-bls-proposal.md §5 (original
threat notes), b3-cross-chain-finality-v1.md (release-leg protocol),
b3-product-identity.md.

## 0. Current decentralized trust model

A deposit mints on B3 iff ALL of:
1. one bootstrap checkpoint (a human/release trust decision, made once),
2. a supermajority (>= 342/512 default) of Ethereum's sync committee signed
   the finalized header, with every committee handover proven back to (1),
3. keccak/SHA-256 links from that header to the deposit event bytes
   (finality branch, execution branch, ancestry chain, receipts MPT), and
4. B3 consensus accepts the bounded type-10 proof and applies the exact mint
   and accounting rules.

A decentralized withdrawal releases on Ethereum iff a quorum (floor(2W/3)+1 by stake
weight) of B3's Modern-PoS validator set signed the finality certificate,
with set handovers proven back to the genesis set pinned in the verifier
contract, and the burn proves into the withdrawal tree.

The current verifier/vault design has no custodian, multisig, oracle, upgrade
admin, rescue key, or arbitrary token selector. Threats are attacks on either
consensus system, the pinned trust roots, the bootstrap procedure, or the code.
The historical managed-v1 operator risks remain relevant only to the excluded
smoke vault and are retained above as a migration/audit record.

## 1. Deposit leg (ETH -> B3)

### T1. Corrupt sync-committee supermajority — CRITICAL, expensive
342 of 512 colluding committee members can sign a fake "finalized" header
containing a fake receipts root -> arbitrary fake deposits -> unlimited
B3-side mint. Committee seats are sampled from ~1M validators, so a
supermajority of seats requires roughly a supermajority of all staked ETH
(tens of billions of dollars) or a catastrophic randomness failure; signing
a conflicting finalized checkpoint is also slashable equivocation.
Mitigations in code: finalized-only (never attestation-weight headers),
supermajority threshold (342, pinned by the independent bridge-readiness gate).
Mitigations in code also include per-block/per-epoch mint-cap enforcement so
even a total committee compromise has bounded damage before humans react. The
exact production caps are still unpinned; optional watcher veto remains a
separate decision. **Do not ship minting without reviewed cap pins.**

### T2. Bootstrap checkpoint poisoning — CRITICAL, cheap if process is weak
Everything chains from the pinned checkpoint root. A wrong/poisoned pin in
a release = attacker-controlled Ethereum view for every B3 node. This is a
process threat, not a code threat.
Required rule: the Ethereum bootstrap checkpoint is pinned in a reviewed
bridge-activation release commit,
cross-checked against multiple independent beacon sources; the same "four
release gates" discipline as H/X pinning.

### T3. Long-range / stale-client attack — HIGH
Validators who have since exited (nothing left at stake) still hold keys
that once formed committee supermajorities. A client that has been offline
longer than the weak-subjectivity window can be fed a fabricated committee
succession built from those dead keys.
The mint path enforces the configured maximum sync lag and fails closed when
the finalized execution head is stale. Halting is safe; catching up through a
fabricated history is not. The production lag value, checkpoint procedure, and
any re-bootstrap procedure remain release/operations decisions.

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
Consensus now enforces the pinned fork schedule's valid-through epoch for
bootstrap, updates, and minting. The reviewed production schedule and upgrade
procedure remain release gates.

### T6. Relayer censorship / liveness — LOW (delay only)
Relaying is permissionless; anyone can submit updates and proofs. A
censoring relayer delays deposits, never forges them. One update per
committee period must reach B3 or catch-up needs archived updates (beacon
nodes serve historical periods). Denial = delay, not loss.

### T7. Vault contract defects — HIGH until audited
Current target: a new immutable single-token vault with no owner, rescue key,
proxy, pause, or upgrade path. It uses a reentrancy guard, actual balance-delta
accounting for incoming fee-on-transfer behavior, an immutable maximum received
amount per deposit, and USDT-compatible optional return handling. Release needs
an inclusion proof under a bridge-qualified staker-finalized root and records the
withdrawal ID before transfer. Residual: single-author review only. **A
third-party audit is a hard gate before non-trivial TVL** (part of independent
bridge readiness, not A3). USDT can still freeze the vault or alter token
semantics; a malicious or broken token implementation can strand reserves.

### T8. Malicious/weird tokens — CONTAINED only after adapter gate
The current vault accepts only its immutable token, but that token still
controls its own `balanceOf` and transfer behavior. The B3 asset registry is
the second firewall: consensus requires the exact ACTIVE
origin/vault/token/asset/decimal/adapter tuple. However, the current path does
not prove that the adapter commitment applies to Ethereum implementation/code
state at the deposit block. Rebasing/deflationary tokens must never be
admitted without an enforced adapter. Registration and adapter semantics are
owner-ruled consensus acts.

### T9. Proof-layer parsing defects — MEDIUM, in OUR code
The strict decoders (canonical RLP, MPT inclusion, SSZ, receipt/envelope,
ancestry) are exactly where forged-bytes bugs would live. Current
assurance: strictness-first implementations, negative tests for every
malformation class we conceived, real-mainnet anchoring (fixtures refuse
to emit unless independently rebuilt roots match Ethereum consensus).
Consensus wiring is now present. Required before activation: fuzz harnesses
over RlpDecode / VerifyMptProof / DecodeReceipt / VerifyExecAncestry / SSZ
branch verification, and an external audit of the verification headers and
state-machine integration.

### T10. Double-mint / replay — IMPLEMENTED, audit still required
Consensus carries a per-origin nullifier keyed by
`(origin_chain_id, vault_address, deposit_id)`, rejects a second mint, and
reverses/rebuilds that state on disconnect/reindex. It also requires the exact
RECIPIENT_V1-derived OWNER output. The mandatory signed policy-9 record
commitment prevents a relayer from changing the MPA mint/output binding or
withdrawal recipient after standard `SIGHASH_ALL` signing. Independent
adversarial review and restart/reorg tests remain activation gates.

### T11. Resource exhaustion — LOW
The canonical type-10 codec bounds record, branch, ancestry, and MPT sizes
before allocation (committees remain fixed at 512). A bridge record consumes
the full 12,000-unit per-transaction MPA verification budget and is exclusive
within its transaction. Independent worst-case benchmarks/fuzzing remain
required before production activation.

## 2. Current decentralized release leg (B3 -> ETH)

### T12. Small early validator set — THE key activation threat
MIN_FINALITY_SET = 2 is a chain-bootstrap floor, NOT bridge security. If
the Ethereum verifier is activated while B3's staked set is small/cheap,
buying 2/3 of B3 weight is cheap and the vault's ENTIRE reserve is the
prize (sign a fake certificate + withdrawal root, drain via §6 proofs).
The decentralized verifier therefore remains closed below its separately
pinned four-validator and total-stake floors. B3 consensus and the Ethereum
verifier both require the same >2/3 signer headcount and >2/3 stake-weight
quorum; one high-weight validator cannot create a B3 certificate for Ethereum
to consume.

### T13. Verifier-contract correctness — HIGH until audited
B3FinalityVerifier + BlsCertificateProver (EIP-2537 pairing path,
ordered depth-13 absent-member paths, bitmap complement/headcount checks,
epoch monotonicity,
withdrawal-tree released[] map) must byte-match the C++ side. The §8
shared test vectors exist as a requirement precisely for this; generate
them from the C++ implementation and run both sides in CI. The immutable
64-member bridge ceiling now has a real post-Fusaka Osaka vector at minimum quorum:
18,144 proof bytes, 18,724 submit calldata bytes, 5,513,351 measured gas for
the complete successful verifier call, and a conservative 6,283,311 total
below the EIP-7825 transaction cap.
Larger current or successor sets close bridge readiness. External cryptography
and contract audit remains required before mainnet activation.

### T14. B3 finality bugs feeding the bridge — bounded by fail-closed
The epoch machinery (handover-gated rotation, extension, lineage breaks)
already fails closed on B3. The verifier mirrors this: no certificate, no
new root or release authority. Stuck > MAX_EPOCH_LAG rejects every later
certificate. Each epoch also has an absolute genesis-time window, and accepted
handovers must be at least `MIN_EPOCH_DURATION` apart; the relative rule stops
already-elapsed epoch numbers from being batch-walked after a relay delay.
Honest catch-up therefore takes one real interval per missed rotation. These
bounds do not defeat a classic long-range lineage that compromised historical
keys keep current in real time; live honest relaying and a reviewed checkpoint
remain assumptions. The vault closes deposits whenever initialization,
certificate freshness, or current/successor-set qualification is absent.
Withdrawals and B3 burns remain disabled until these canonicality and liveness
risks are solved rather than merely disclosed.

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
  token decimals, B3 kB3 1e9). Consensus now uses explicit exact power-of-ten
  conversion and rejects rounding/overflow; canonical bUSD is 6-to-6 raw
  units. The known RPC
  parse/display quirk (1e8 vs 1e6) must NOT leak into bridge math.
- **Monitoring**: even with perfect code, run watchers comparing vault
  reserves vs B3-minted supply per asset; alert on divergence. Cheap, and
  it converts several CRITICALs into detected incidents.

## 4. Ranked gates before real value flows

1. Review and pin the bridge-specific B3 validator-count and total-weight
   thresholds (T12), including the 64-member immutable contract ceiling.
2. Independent review and hardening of the implemented type-10 state machine,
   exact mint/burn transitions, nullifier/caps, undo/reindex replay, recipient,
   sync-lag, and unknown-fork refusal. Add a durable bridge sidecar before
   allowing pruning; until then preserve the explicit prune/snapshot refusal.
3. Implement and exercise the origin-enforced USDT adapter semantics and
   upgrade invalidation rule (T8).
4. Rehearse the permissionless certificate and withdrawal-proof relay end to
   end, including chronological epoch catch-up, duplicate-id rejection, gas
   funding, relayer restart, and fail-closed/pruned-node behavior.
5. Third-party audits: vault contract, C++ verification headers, Solidity
   verifier/prover, and the C++/Solidity encoding seam (T7, T9, T13).
6. Fuzzing of every byte-level decoder (T9).
7. §8 shared vectors generated and run against both implementations (T13).
8. Checkpoint-pinning release process, same rigor as H/X (T2).
9. Reserve to supply watcher tooling (§3 Monitoring).

The historical 0.001-ETH smoke transaction was exempt from these gates by
proportionality. It does not authorize promoting that vault. Production use of
the new verifier/vault remains fail-closed until every applicable proof, cap,
audit, bootstrap, runtime-code, chainparams, and operating-rule gate passes.


## 5. Consensus mint admission rules — OWNER-RATIFIED; core path implemented

Historical review of `B3DepositVault.sol` identified the token-admission and
token-semantics boundary: a finalized receipt proves the vault emitted an
event, never that token code reported a truthful balance. The current
`B3StakerBridge` immutably selects one token and uses net balance-delta
accounting, but canonical token implementation/adapter behavior still must be
pinned and enforced. The following remain binding requirements on the stage-4
consensus design (they refine T8/T10 into normative rules):

1. **Burn = PROPOSED, never approval.** The B3 issuance-fee burn moves a
   token only into a PROPOSED registry state; it is an anti-spam fee.
   Activation is a separate, explicit approval act.
2. **Mint requires an ACTIVE registry entry** binding the full tuple:
   `(ethereum_chain_id, vault_address, token_address, b3_asset_id,
   decimals/raw-unit conversion rules, implementation/adapter version,
   approval block range)`. No tuple, no mint — regardless of proof
   validity.
3. **Exactly-once consumption**: the nullifier key is
   `(chain_id, vault_address, deposit_id)`, consumed once, with the
   EXACT log-emitting contract address verified against the registry
   (already enforced at the verification layer: ExtractDeposits filters
   on the precise vault address).
4. **Token-semantics gate**: rebasing, reflection, blacklistable,
   clawback-capable, fee-charging and upgradeable tokens are rejected or
   admitted only through explicit per-token adapters. A proxy address is
   not a stable security identity — approval binds implementation/
   adapter version, and an upgrade voids the entry until re-approved.
5. **Withdrawals derive the Ethereum token from the approved B3 asset
   mapping** — never from user-provided token data.

Rules 2, 3, exact recipient/amount conversion, caps, and exact OWNER output are
implemented by the type-10 mint path. The registry stores and hashes an
implementation/adapter commitment but does not yet prove or enforce that
commitment against Ethereum token code/state at the deposit block. A nonzero
placeholder is not an adapter and must never satisfy production readiness;
the adapter-v1 commitment preimage, origin-block applicability, and upgrade
invalidation rule still require implementation and tests.

## 6. Pre-activation replacement versus later asset migration

The historical managed vault has no pause, proxy, owner rotation, or authority
replacement. The current decentralized target therefore uses a new audited
vault. Under `BridgeAssetIdV1`, `vault_address` is part of the asset identity,
so that new vault has a new `AssetId`; it cannot silently continue the old
bUSD namespace.

In the current pre-activation case, recheck that the old vault has no USDT
liability and that no consensus bUSD has been minted, then pin only the new
vault/AssetId and exclude the old one. No user balance exists to migrate under
those facts. If a managed vault is ever activated or gains liabilities first,
a reviewed migration must pin the old registry's last B3 approval height, stop
old-vault deposit presentation, wait through the origin/B3 finality drain
window, and refund or explicitly handle deposits that arrive after cutoff (the
old contract remains callable). Existing bUSD must then be burned or swapped
and reissued against reconciled reserves without creating a second mint claim.
