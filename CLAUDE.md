# B3Coin Core — Repository Context

## Branch and audit scope lock (owner ruling 2026-09-03)

- The authorized Claude workspace is `/Users/josh/development/ON/B3-FlowMesh`,
  checked out on branch **`FlowMesh`**. Verify both before any write or commit;
  if either differs, stop and report it.
- This branch carries the complete v1.1.1-beta.2 transition and FlowMesh code
  for independent audit. Review the current implementation as one integrated
  system and report findings against the exact checked-out revision.
- Do not switch branches, create another worktree, merge another branch, push,
  or rewrite history. Do not change consensus or release parameters unless the
  project owner explicitly asks for that change after reviewing the audit.

B3Coin is **one continuous blockchain**. This tree is a Bitcoin Core 31.1 fork that
carries the original B3 chain through a two-era design: a **legacy era** (the existing
B3Coin PoS chain, preserved exactly) and a **modern era** (clean Core-31.1-style
consensus) that begins at a single, immutable transition boundary.

## The core invariant

> B3 has one immutable historical ledger through block **X** at height **H**;
> beginning with **H+1**, the same ledger evolves under a new consensus and
> state-transition language **without rewriting any historical identity**.

Every change must be tested against that statement.

## Authority and precedence

Direct, explicit project-owner decisions are the highest authority. When such a
decision conflicts with tracked governing documentation, implementation must stop
until the documentation is reconciled in a reviewed commit. Untracked or
unreviewed working-tree documents are proposals only and cannot acquire governing
authority merely by declaring precedence. The persistent order is:

1. Latest explicit project-owner ruling
2. Reviewed and committed architecture contract / master handoff
3. Reviewed subordinate design documents
4. Implementation assumptions

## Authoritative documents (read before consensus-adjacent work)

- **[doc/design/b3-master-handoff.md](doc/design/b3-master-handoff.md)** — the **top
  authority**. The complete project concept: one chain, three economic roles (B3+STAKE
  secures the base chain, FN Coin operates FlowMesh, registered assets trade against
  native B3 and FlowMesh fees are native B3), the transition corridor, colored assets,
  FN Coin / Proof of Disintegration,
  the FlowMesh DEX, microblocks, the 12-step build order, and Claude's operating
  contract. Where it disagrees with any other document here, **it governs** — subject
  only to its own §0 precedence order (the owner's later explicit corrections outrank
  it). Read it before any consensus-adjacent work.
- **[doc/design/b3-master-handoff-conflicts.md](doc/design/b3-master-handoff-conflicts.md)** —
  every known disagreement between the master handoff and the older documents, reported
  rather than silently resolved. Check it before trusting a "LOCKED" label anywhere else.
- **[doc/design/b3-architecture-contract.md](doc/design/b3-architecture-contract.md)** —
  the locked architecture contract. Authoritative over all earlier design notes, and
  still binding in every area the master handoff does not contradict.
- **[doc/design/b3-open-decisions.md](doc/design/b3-open-decisions.md)** — decisions
  that are **not yet locked**, plus an explicitly marked historical register
  of items closed by later owner rulings. Modern PoS V1 and the sealed
  transition/A1/A2/A3 parameters are implemented and pinned; do not reopen or
  redesign them while working on the genuinely open items.
- **[doc/design/b3-implementation-status.md](doc/design/b3-implementation-status.md)** —
  the implementation-status / gap matrix: what is LOCKED / IMPLEMENTED / PARTIAL / WRONG /
  MISSING / SECURITY-BLOCKER, and the minimal critical path to a clean H+1.
- **[doc/design/b3-fn-assets-activation-design.md](doc/design/b3-fn-assets-activation-design.md)**
  and **[doc/design/b3-flowmesh-v1-production.md](doc/design/b3-flowmesh-v1-production.md)** —
  the current transition-release contracts for FN Genesis, simple assets and production
  FlowMesh. These implement later explicit owner rulings and therefore supersede older
  documents that defer those features to another release.
- [doc/design/b3-legacy-fork-choice.md](doc/design/b3-legacy-fork-choice.md) — how a
  legacy PoS block earns chain weight, traced from the historical `master` client. The
  reference for legacy-era anti-DoS work.
- [doc/design/b3-era-architecture.md](doc/design/b3-era-architecture.md) and
  [doc/design/b3-test-baseline.md](doc/design/b3-test-baseline.md) — supporting
  background (historical spike + test baseline). Non-authoritative where they differ
  from the contract.

## Governing rules for implementation

1. **Do not silently alter the locked architecture to solve an implementation problem.**
   If code contradicts the contract, **report the contradiction** — do not choose a new
   protocol.
2. **Sequencing (do not skip):** preserve the implemented H/X transition and Modern-PoS
   rules. FN PoD, assets and FlowMesh remain separately height-gated at A1/A2/A3; an
   incomplete mainnet schedule must fail closed.
3. **Modern PoS:** the reviewed V1 implementation is present. Do not replace or redesign
   it while implementing later activation-gated features.
4. **Genesis is permanent.** Never regenerate it, change its bytes/nonce/bits/time/merkle/
   hash, apply the modern marker to it, or reinterpret historical blocks with the modern
   codec. If a task appears to require any of these, stop and report.
5. **Bridge-backed bUSD (latest owner ruling):** bUSD is the B3 representation of USDT
   locked through the Ethereum bridge, not the earlier CDP proposal and not an
   unrestricted fixed-supply token. The managed-v1 vault is exactly
   `0x143F207e23e6aebD7E974be90ac6D434f4c7BFb6`, with Ethereum-mainnet
   canonical USDT `0xdAC17F958D2ee523a2206206994597C13D831ec7` and exact six-decimal
   conversion. Authority, runtime code, bootstrap, caps, adapter, persistence,
   and activation pins must all pass independent review before mainnet minting.
6. **Managed withdrawal leg for transition v1 (owner ruling 2026-09-01):** the
   already-deployed vault's immutable owner authority is intentionally used for
   withdrawals in this release. Label it honestly; do not describe the release
   leg as decentralized. Pin the authority and runtime code from independent
   mainnet observations before activation. A future verifier requires a new
   vault and explicit reserve migration because this authority is immutable.
7. **Small, buildable commits.** One logical change per commit; each must build and be
   independently reviewable.

## Git rules

- Work only in `/Users/josh/development/ON/B3-FlowMesh` on the already checked-out
  branch **`FlowMesh`**. Do not switch branches, create worktrees, or merge another
  branch into it.
- Do not push, amend, squash, reset, rebase, or rewrite history. Do not modify previous
  commits.
- **Never** add AI/assistant attribution to commits (no "Claude", "Anthropic",
  "Generated-By", "Co-Authored-By", etc.). Use the repository Git identity only.

## Safety constraints (development environment)

- Do not connect to production B3 nodes and do not use a real B3Coin datadir. `ChainType::MAIN`
  **is** B3 production (magic `b3 2e 1e e6`, port 5647, live seeds) — network-touching
  verification must be regtest-with-no-peers or unit tests only.
- Do not install dependencies. Report proposed files + design before editing on
  architecture tasks; afterwards report modified files, build/test commands + results,
  `git diff --stat`, unresolved conflicts, and prototype-only parts.

## Where the code lives

- `src/consensus/` — era selection (`era.h`), block-codec marker (`block_codec.h`),
  transition boundary (`boundary.h`). Wired and live.
- `src/legacy/` — legacy consensus (PoS kernel, stake modifier, rewards, difficulty, FN
  collateral), legacy codec, and `TrustedReplay` (historical UTXO reconstruction).
- `src/modern/` — modern data models and the live, activation-gated FN/assets/FlowMesh
  carrier and validation rules.
- `src/flowmesh/` and `src/node/flowmesh_*` — production execution, BLS-certified log,
  persistence, chain indexes, P2P runtime and service. FlowMesh uses existing B3 P2P and
  must remain fail-independent from base-chain liveness.
- `src/qt/` — B3/FlowMesh UI shell. Renders no fabricated data; does not touch consensus.
