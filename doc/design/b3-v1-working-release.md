# B3 Hive 1.0.0 — working release (safe, not perfect)

Owner directive 2026-08-25: "pack a working release that's safe, not
perfect but safe." This manifest defines that pack: what is in it, why it
is safe to run, what works today with evidence, and exactly what is
dormant pending owner pins. The product is **B3 Hive**; the network and
protocol are **B3 FlowMesh**. Built from branch
`test/b3-clean-architecture`.

## What "safe" means here — the fail-closed contract

The candidate retains normal legacy wallet and node behavior, including
ordinary legacy transactions. Its safety claim is narrower: unfinished
modern, FlowMesh and bridge paths cannot activate on mainnet.

| Guard | State in this pack |
|---|---|
| Mainnet boundary | **PINNED, PAUSE-FAIL-CLOSED.** Final legacy height H is 810,000 (`hard_fork_height = 810001`). X is deliberately blank, so this binary accepts the legacy chain through H and refuses every block at 810,001. It cannot enter the corridor or Modern PoS. |
| X-pin discipline | Per the 2026-08-23 owner ruling, the mandatory follow-up release records the observed hash X of block 810,000 and supplies the reviewed Modern-PoS parameter block. Blank-X nodes never enter the corridor. |
| Genesis / history | Untouched and untouchable (contract rule 4). |
| Bridge | `b3-bridge-ethcheck` is an optional developer verification tool and is excluded from this package. Nothing bridge-related is reachable from consensus; deposits cannot mint. |
| FlowMesh DEX / assets / FN | Libraries, tests and honest inactive UI surfaces exist, but production consensus activation is absent (MPA creation actions 1–3 remain inactive). |
| Modern test network | The transition, Modern PoS and BLS finality are exercisable on section-64 modern regtest (`-regtest -b3modernregtest`), a local throwaway network. This does not activate unfinished product features. |

Safety is therefore structural: the risky code paths are not "believed
correct", they are unreachable on real networks.

## Package contents

`contrib/packaging/make_v1_package.sh` produces a **B3 Hive 1.0.0** explicitly unsigned
release-candidate archive plus its SHA-256 checksum:
`dist/b3-hive-v<version>-unsigned-rc-<arch>-<os>.tar.gz`. The helper first
rebuilds all six packaged targets in the selected CMake build directory, then
validates that the daemon version matches `CMakeLists.txt`, fails if any
required program is missing, and refuses to overwrite an existing candidate.

- `bin/` — `b3coind` (B3 FlowMesh client daemon), `b3coin-cli`,
  `b3coin-tx`, `b3coin-util`, `b3coin-wallet`, and `b3coin-qt` (**B3 Hive**).
  Bridge, benchmark, and test programs are deliberately excluded.
- `demo/` — `start_network.sh` / `stop_network.sh`: a four-validator local
  B3 FlowMesh network (corridor → Modern PoS → live BLS finality) with a
  printed command to attach B3 Hive to it.
- `README.md`, `COPYING`, `doc/` — public project overview, MIT license, and
  product identity.
- `UNSIGNED-RELEASE-CANDIDATE.txt` — a prominent warning that the archive is
  for candidate testing and is not a signed, notarized public release.

The helper archive and the CI-generated platform artifacts are **unsigned
release candidates**, not public releases. Source version `1.0.0`, a checksum,
or an ad-hoc application signature does not authenticate a release. Public
distribution is a separate promotion step requiring the matching `v1.0.0`
tag, audited/reproducible candidate results, authenticated checksums and the
applicable platform signing/notarization. The update client also remains
quiet and fail-closed until its HTTPS endpoint, allowed hosts and threshold
release keys are pinned in a reviewed release build.

The local helper archive is a same-environment engineering package. On macOS,
the deploy-generated `.app` zip is the runnable candidate because it contains
the required Qt frameworks; public Linux artifacts must come from the reviewed
reproducible release toolchain rather than a workstation's shared libraries.

## What demonstrably works (evidence)

1. **Legacy chain correctness** — TrustedReplay, legacy PoS validation,
   real-history U==U′ equivalence campaign PASSED 2026-08-22 (T1/T2/T3 all
   EQUAL; doc/design/b3-utxo-equivalence-runs.md).
2. **Modern PoS + BLS finality** — 4-validator multi-node soak
   (feature_b3_finality_soak.py): corridor, stake+key binding, quorum
   finality, certified epoch rotation, persisted pin, restart recovery,
   partition reorg. A fresh release-candidate soak passes. The full canonical
   release gate is rerun for every candidate; this manifest does not reuse a
   stale suite count.
3. **B3 Hive** — builds and runs with the locked product identity
   (`B3 Hive version v1.0.0`) and the B3 mark as its application,
   window and notification icon. Its designed shell includes the dashboard,
   assets, trade, stake and settings pages. The dashboard and assets surfaces
   use the existing wallet/node models and existing send/receive actions;
   settings organizes existing options and wallet-security actions. Trade is
   an honest preview with a null backend (submission disabled), while Stake
   shows real wallet state and reward history but has no Qt staking backend.
   A fresh warnings-as-errors Release build and the complete offscreen Qt test
   executable pass.
4. **Bridge deposit leg (ETH → B3 verification)** — proven on Ethereum
   MAINNET with real value 2026-08-25: vault
   `0x143F207e23e6aebD7E974be90ac6D434f4c7BFb6`, deposit id 0 (0.001 ETH,
   tx `0x98321803…ba475d`) verified end to end (committee signatures,
   finality, 22-header ancestry, receipt proof, event extraction:
   "DEPOSIT PROVEN … ALL VERIFIED"). Minting is stage-4, gated.
5. **Legacy mainnet wallet path** — tiny-value receive, legacy P2PKH-only
   address policy, Hive-to-old-network send, confirmation, nTime-preserving
   raw round-trip and balance/fee display all passed on 2026-08-27. The
   transaction identifiers and block evidence are recorded in
   `doc/b3-v1-mainnet-qa.md`.

## Known-imperfect in the unsigned candidate

- The Trade workspace is visually implemented but cannot place an order in
  this build; its production backend remains intentionally absent.
- The Stake page reports real wallet state and rewards, but staking controls
  remain backend-inert in Qt. Existing engine RPCs include `startstaking`,
  `getfinalitystatus`, `bindfinalitykey` and `createstake`.
- The Settings update section is present, but public release URL/host/key
  pins and native replacement installers are not configured; it therefore
  reports the disabled state and performs no network update operation.
- The bridge threat model's §4 gates stand unstarted (audits, fuzzing,
  stage-4 rulings) — irrelevant to this pack's safety because nothing
  bridge/mint-related is live, mandatory before any activation release.
- A candidate archive remains unsuitable for public distribution until the
  remaining signing, notarization, reproducibility and publication gates below
  are complete. The required tiny-value mainnet wallet smoke has passed.

## Follow-up and public-release gates

Already owner-ruled and pinned here: H=810,000; corridor
810,001..811,000; M=811,001; 1,000 corridor blocks; compact bits
`0x1f008000`; 60/120-second pacing; and a fee-only corridor. The modern
reward mechanism is also owner-ruled, but its exact initial amount depends
on the measured supply at H and is not configured in this blank-X binary.

The X-pin/Modern-PoS follow-up still requires the observed X at H, the final
H U==U′ capture, and a complete reviewed mainnet Modern-PoS parameter block
(including every still-open value). Network release operations still require
at least two additional independently hosted fixed seeds plus an
owner-controlled DNS seed.

Promotion from an unsigned candidate to a public B3 Hive 1.0.0 release still
requires the matching release gate on the exact candidate, frozen-vector
comparison on the release platforms, reproducibility/audit, authenticated
distribution, and platform signing/notarization credentials. The recorded
tiny-value mainnet send/receive smoke passed on 2026-08-27. The X-pin follow-up
remains a separate deliberate release. Nothing in this candidate silently
supplies X or resolves an open owner decision.
