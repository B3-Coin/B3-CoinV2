# FINALIZED — the owner's ledger of final state

Advanced 2026-08-27 on the owner's instruction, folding in the 94 commits
of the release push (Modern PoS qualification through the Codex
reconciliation). This branch records what is FINAL; open items live in
doc/design/b3-open-decisions.md.

## Product
- Identity LOCKED: **B3 FlowMesh** (platform) / **B3 Hive** (desktop app,
  version **1.0.0**) / **B3** (asset). B3Coin = legacy/compat identifiers
  only. Hive mark: outline hexagon + B3 monogram (simple version ruled).
- Default datadir = the legacy `B3-CoinV2` locations verbatim; legacy
  wallet.dat migrates in place (doc/b3-wallet-migration.md).

## Chain
- One chain: legacy ≤ H, PoW corridor, Modern PoS from M. **H = 810,000
  PINNED in mainnet chainparams** (hard_fork_height = 810,001 per the
  field's first-non-legacy semantics; corridor 810,001..811,000 at bits
  0x1f008000; **M = 811,001**), expected ~2026-09-01. X deliberately
  blank: the OD-10 pause-fail-closed v1 shape, verified by
  finality_activation_tests; the X-pin release adds the observed hash.
- Modern PoS V1 + BLS finality: implemented, multi-node soak-qualified,
  persisted fail-closed pin; F = M. Full mainnet replay from genesis
  against live peers PASSED (height 808,751, zero errors); real-history
  U==U′ campaign PASSED.
- Fail-closed everywhere until the X-pin release: mainnet today validates
  the legacy chain only.

## Economics (OD-2 CLOSED 2026-08-26)
- Emission: R0 = floor(S_H × 1% / 525,600) at M, halving every 525,600
  blocks (one year); corridor = 0 + fees; split 90% producer / 10%
  treasury enforced in the coinbase; verified live on a rehearsal chain.
- Treasury: ONE wallet, `SNyANHiUkuqPSfbeKHDXzVD86LC2ZUUjLX` (pinned);
  issuance fees and FlowMesh fees (0.01% flat, 80/20 FlowMesh/treasury)
  pay the same address.
- Denomination: 1 B3 = 1e9 base units, the ONLY human-facing unit
  (RPC, GUI, config); typed amount = staking weight.
- FN Coin: lifetime cap 5,000 (ratified); at least 3,500 historical rights
  reserved from the height-807,709 report, with the exact final reservation
  set by the mandatory through-H report; PoD burn; modern creation cost
  pinned by slot to 15,000 / 30,000 / 60,000 B3 per 500-slot tier. The
  helper ships activation-inert until the reviewed FN activation path exists.

## Bridge
- Deposit legs first (ETH → B3, then BTC → B3). Verification stack
  (RLP/MPT/SSZ/light client/ancestry/receipt-decode) mainnet-proven end
  to end, including a REAL 0.001 ETH deposit through the deployed vault
  `0x143F207e23e6aebD7E974be90ac6D434f4c7BFb6` (DEPOSIT PROVEN).
  Verification-only in v1: no consensus reach; stage-4 mint admission
  rules ratified (threat model §5); b3Recipient encoding proposed.

## Legacy-era wallet safety (release reviews, closed)
- Send + receive is the ruled legacy-era scope. Receive: legacy P2PKH
  only -- witness address types refused at the handout choke point and in
  every RPC/GUI path (SegWit inactive = anyone-can-spend). Send:
  historical nTime identity end to end (era judged at the active tip,
  fail-closed if unavailable), witness recipients AND explicit witness
  change refused, raw-tx RPCs round-trip the legacy encoding.

## Application
- B3 Hive 1.0.0: redesigned shell (Codex pass, reconciled), secure
  update system (threshold-signed manifests, hardened rollback state,
  honest install gating: v1 = notify + verified download), wallet ops
  verified on a live network, marker-aware RPC block display.
- Release tooling: b3hive-sign (offline manifests), treasury wallet
  generator (node-verified derivation), demo network scripts, v1
  packaging, GitHub Actions release builds (linux gate + windows cross).

## v1.0.0 RELEASE CANDIDATE (2026-08-28)
Live mainnet QA PASSED: old-client funding received, displayed and
persisted by Hive; a Hive-built nTime-signed legacy send CONFIRMED in
legacy block b62539433b… at height 809,011. Four production bugs found
by QA and fixed with regression tests (wallet legacy-tx persistence, fee
floor, fallback default, -maxtxfee denomination); legacy-era fee UI
simplified to a fixed fee by owner ruling (smart fees activate at H).
External audit closed: Werror-clean build, script_tests regression fixed
(fully green), true CI hard gate, unsigned Linux/Windows/macOS release
candidates produced by CI. Remaining before publication: owner signing
keys + manifest URL (updater ships disabled without them), artifact
signing/notarization, push + Actions run, operator distribution before
H = 810,000 (~Sep 1); then the X-pin release.

## Remaining before the X-pin release (not blockers of THIS ledger state)
X + S_H at 810,000 (~Sep 1); seeds list; owner release signing keys;
GitHub Actions first run (linux vectors + windows); operator
distribution before H.
