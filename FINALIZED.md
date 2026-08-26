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
- One chain: legacy ≤ H, PoW corridor, Modern PoS from M. **H = 810,000**
  (corridor 810,001..811,000, **M = 811,001**), expected ~2026-09-01; X
  pinned from the observed block per §62.
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
- FN Coin: lifetime cap 5,000 (ratified); PoD burn; nondecreasing modern
  creation curve (numbers open).

## Bridge
- Deposit legs first (ETH → B3, then BTC → B3). Verification stack
  (RLP/MPT/SSZ/light client/ancestry/receipt-decode) mainnet-proven end
  to end, including a REAL 0.001 ETH deposit through the deployed vault
  `0x143F207e23e6aebD7E974be90ac6D434f4c7BFb6` (DEPOSIT PROVEN).
  Verification-only in v1: no consensus reach; stage-4 mint admission
  rules ratified (threat model §5); b3Recipient encoding proposed.

## Application
- B3 Hive 1.0.0: redesigned shell (Codex pass, reconciled), secure
  update system (threshold-signed manifests, hardened rollback state,
  honest install gating: v1 = notify + verified download), wallet ops
  verified on a live network, marker-aware RPC block display.
- Release tooling: b3hive-sign (offline manifests), treasury wallet
  generator (node-verified derivation), demo network scripts, v1
  packaging, GitHub Actions release builds (linux gate + windows cross).

## Remaining before the X-pin release (not blockers of THIS ledger state)
X + S_H at 810,000 (~Sep 1); seeds list; owner release signing keys;
GitHub Actions first run (linux vectors + windows); operator
distribution before H.
