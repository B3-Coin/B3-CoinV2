# B3Coin Core — Repository Context

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
  secures the base chain, FN Coin operates FlowMesh, approved stablecoins denominate
  trading), the transition corridor, colored assets, FN Coin / Proof of Disintegration,
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
  that are **not yet locked**. Notably, **modern PoS is UNRESOLVED at the protocol-detail
  level** and must not be implemented until its consensus specification is supplied.
- **[doc/design/b3-implementation-status.md](doc/design/b3-implementation-status.md)** —
  the implementation-status / gap matrix: what is LOCKED / IMPLEMENTED / PARTIAL / WRONG /
  MISSING / SECURITY-BLOCKER, and the minimal critical path to a clean H+1.
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
2. **Sequencing (do not skip):** reach a clean, produced-and-validated **H+1** before
   wiring any of FlowMesh, FN, bridges, or advanced/coloured assets into consensus.
   Those are Phase-3 features gated behind later activation heights (A1/A2/A3).
3. **Modern PoS:** not implemented until its consensus spec is supplied
   (see open-decisions). It currently fails closed by design.
4. **Genesis is permanent.** Never regenerate it, change its bytes/nonce/bits/time/merkle/
   hash, apply the modern marker to it, or reinterpret historical blocks with the modern
   codec. If a task appears to require any of these, stop and report.
5. **Small, buildable commits.** One logical change per commit; each must build and be
   independently reviewable.

## Git rules

- Work on branch **`test/b3-clean-architecture`** (renamed from `claude/b3-clean-architecture`
  on 2026-08-23 at the owner's request; branched from `a8ad010`, the tip of
  the completed experimental stack — **not** from the older `claude/b3-full-architecture`
  checkout, which carries defects fixed later).
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
- `src/modern/` — modern data models (policy outputs, transition proofs, assets, vault,
  PoS dispatch). Header-only; only `modern/pos.h` is reachable from validation, and it
  fails closed.
- `src/flowmesh/` — DEX execution models (ledger, clearing, batch). Header-only,
  test-only. **Not** wired into consensus.
- `src/qt/` — B3/FlowMesh UI shell. Renders no fabricated data; does not touch consensus.
