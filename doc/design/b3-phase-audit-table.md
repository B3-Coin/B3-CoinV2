# Phase-awareness audit table

**As of the corridor hardening pass (2026-08-16).** Every path that once
assumed two consensus states (legacy/modern), classified against the
three-phase model `LEGACY_POS / TRANSITION_POW / MODERN_POS`.

Classification key: **FIXED** = phase-aware now · **TWO-STATE** =
intentionally two-valued because the path concerns block/transaction FORMAT
(the era dimension), which genuinely has two values · **DEFERRED** = awaits
the Modern PoS specification.

| Path | Previous assumption | Correct phase behavior | Classification |
|---|---|---|---|
| Block production dispatch (`ConnectBlock` production branch) | modern ⇒ modern PoS gate | LEGACY_POS live/replay rules; TRANSITION_POW coinbase cap only (proof earned at header); MODERN_POS fail-closed gate | **FIXED** (corridor stages) |
| Contextual header validation | modern ⇒ stock nBits + SHA256d | TRANSITION_POW: constant corridor bits + scrypt eligibility; MODERN_POS: stock rule as placeholder | **FIXED**; MODERN_POS rule itself **DEFERRED** |
| Context-free `CheckBlockHeader` PoW | marker-modern ⇒ SHA256d | With a corridor configured, marker-modern headers defer to the contextual check (no height context exists) | **FIXED** |
| Block index load (`LoadBlockIndexGuts`) | era MODERN ⇒ SHA256d at restart | Phase switch: legacy deferral / corridor scrypt on the reconstructed header / modern placeholder | **FIXED** (this pass; was the audit's real defect) |
| Restart continuation | untested | Chain continues after teardown + reload | **FIXED** (disk-backed regression test) |
| Reindex (chainstate) | untested through corridor | Full legacy+corridor reconnect to the same tip and registry | **FIXED** (test); full blockfile `-reindex` exercised via the same connect path, not separately scripted |
| Block read integrity (`ReadBlock`) | — | B3 modern blocks: identity vs indexed hash only; header-PoW guard correctly excludes them | **TWO-STATE** (verified correct) |
| Headers-first sync pre-filter (`HasValidProofOfWork`/`CheckHeadersPoW`) | chain-wide `true` on B3 | Corridor headers are header-only scrypt-verifiable; per-phase filtering possible once batch heights are anchored. Full enforcement exists downstream in `AcceptBlockHeader`'s contextual check, so this is a DoS-cheapness optimization, not a correctness hole | **DEFERRED** (with the modern PoS header rule) |
| Claimed-work accounting (`CalculateClaimedHeadersWork`, anti-DoS thresholds, compact-block gating) | — | nBits-derived, hash-agnostic — uniform by design (trust = 2^256/(target+1)) | **TWO-STATE** (by design); corridor low-work implications feed the OPEN difficulty decision |
| Chainwork / fork choice (`GetBlockProof`, candidate selection) | — | Uniform work accumulator across all phases; anchor rules orthogonal | **TWO-STATE** (by design); MODERN_POS fork-choice weighting **DEFERRED** |
| Block templates (`BlockAssembler`) | one template shape | Legacy heights refused; corridor: marker version + corridor bits + fees+reward coinbase; modern-PoS heights refuse via the fail-closed self-check | **FIXED** |
| Template validity self-check | contextual always checks PoW | `check_pow` threaded so ungrounded templates pass their own validity test | **FIXED** |
| Mining/generate grind | SHA256d always | Corridor heights grind scrypt (shared `CheckTransitionPowEligibility`); tested through mempool→template→grind→submit at H+1/H+500/H+1000 | **FIXED** |
| `submitblock` / block decode | — | Modern codec covers corridor blocks; legacy submission unsupported (unchanged) | **TWO-STATE** (format dimension) |
| `getblocktemplate` RPC contract | SHA256d target semantics | Needs a corridor annotation (which hash to grind) for external miners | **DEFERRED** (interface work, pre-mainnet-corridor) |
| Mempool era gate / tx codec selection | two-valued era | Transaction FORMAT genuinely has two values; corridor transactions are modern transactions | **TWO-STATE** (correct) |
| Script flags (`GetBlockScriptFlags`, LEGACY_LOCK per-input override) | two-valued era | Frozen legacy flags for pre-H coins in any MODERN-era phase; modern flags otherwise | **TWO-STATE** (correct; crossing is era-based by design) |
| Stake rules dispatch (`modern::SelectStakeRules`) | two-state LEGACY/MODERN | Production selector is `GetConsensusPhase`; `SelectStakeRules` remains the format×era classifier used by the modern gate | **DEFERRED** (absorbed into the PoS validator interface when specified) |
| RPC era/phase reporting | no phase surfaced | Expose phase (and corridor progress) in chain info RPCs | **DEFERRED** (cosmetic/tooling) |
| Wallet transaction creation | stock modern wallet | Corridor-era wallet builds ordinary modern transactions; no STAKE-creation support; STAKE outputs nonstandard to relay | **DEFERRED** (tooling + the relay carve-in from the checklist) |
| Two-node fresh sync through the transition | untested | Pinned-boundary legacy prefix + scrypt corridor validation; identical tip/UTXO commitment/registry | **FIXED** (test) |
