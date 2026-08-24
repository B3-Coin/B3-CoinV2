# B3 FlowMesh v1 — working release (safe, not perfect)

Owner directive 2026-08-25: "pack a working release that's safe, not
perfect but safe." This manifest defines that pack: what is in it, why it
is safe to run, what works today with evidence, and exactly what is
dormant pending owner pins. Built from branch `test/b3-clean-architecture`.

## What "safe" means here — the fail-closed contract

The shipped binaries CANNOT do anything irreversible to the real B3 chain:

| Guard | State in this pack |
|---|---|
| Modern era on mainnet | **INERT.** `hard_fork_height` (H), `legacy_final_hash` (X) and the Modern-PoS params are unset on every shipping network. The node follows the legacy B3 chain exactly; the modern rules cannot activate. |
| X-pin discipline | Per the 2026-08-23 ruling, activation ships later as a separate release with H set and X blank, which accepts through H and refuses every block at H+1 until the deliberate X pin. This pack is the step before that: fully legacy. |
| Genesis / history | Untouched and untouchable (contract rule 4). |
| Bridge | Verification tooling only (`b3-bridge-ethcheck`); nothing bridge-related is reachable from consensus. Deposits cannot mint. |
| FlowMesh DEX / assets / FN | Header-only or test-only; not wired into consensus (MPA creation actions 1–3 inactive). |
| Modern features | Fully exercisable, but ONLY on the section-64 modern regtest (`-regtest -b3modernregtest`), a local throwaway network. |

Safety is therefore structural: the risky code paths are not "believed
correct", they are unreachable on real networks.

## Package contents

`contrib/packaging/make_v1_package.sh` produces
`dist/b3-flowmesh-v1-<arch>.tar.gz`:

- `bin/` — `b3coind` (B3 FlowMesh client daemon), `b3coin-cli`,
  `b3coin-tx`, `b3coin-wallet`, `b3coin-qt` (**B3 Hive**),
  `b3-bridge-ethcheck` (bridge verification harness).
- `demo/` — `start_network.sh` / `stop_network.sh`: a four-validator local
  B3 FlowMesh network (corridor → Modern PoS → live BLS finality) with a
  printed command to attach B3 Hive to it.
- `README.md` (this file), `doc/` — threat model, product identity.

## What demonstrably works (evidence)

1. **Legacy chain correctness** — TrustedReplay, legacy PoS validation,
   real-history U==U′ equivalence campaign PASSED 2026-08-22 (T1/T2/T3 all
   EQUAL; doc/design/b3-utxo-equivalence-runs.md).
2. **Modern PoS + BLS finality** — 4-validator multi-node soak
   (feature_b3_finality_soak.py): corridor, stake+key binding, quorum
   finality, certified epoch rotation, persisted pin, restart recovery,
   partition reorg. Canonical suite 206/188/18 known-stock at the 2026-08-24
   qualification; consensus untouched since.
3. **B3 Hive** — builds and runs with the locked product identity
   (`B3 Hive version v31.1.0`); attaches to the demo network as an
   observer/wallet node. Functional stake/trade pages are v1.x work.
4. **Bridge deposit leg (ETH → B3 verification)** — proven on Ethereum
   MAINNET with real value 2026-08-25: vault
   `0x143F207e23e6aebD7E974be90ac6D434f4c7BFb6`, deposit id 0 (0.001 ETH,
   tx `0x98321803…ba475d`) verified end to end (committee signatures,
   finality, 22-header ancestry, receipt proof, event extraction:
   "DEPOSIT PROVEN … ALL VERIFIED"). Minting is stage-4, gated.

## Known-imperfect, accepted for v1

- B3 Hive has no stake/trade pages yet (engine RPCs exist:
  `startstaking`, `getfinalitystatus`, `bindfinalitykey`, `createstake`).
- Icons/graphics are stock placeholders pending the owner's visual identity.
- GUI build on macOS needs `-DWITH_QRENCODE=OFF` unless libqrencode exists.
- The bridge threat model's §4 gates stand unstarted (audits, fuzzing,
  stage-4 rulings) — irrelevant to this pack's safety because nothing
  bridge/mint-related is live, mandatory before any activation release.

## The activation release (NOT this pack) still requires the owner pins

X hash at H=820,000; corridor params final (1,000 blocks, bits 0x1f008000,
60/120 s pacing — ruled); `reward` (OD-2); sentinel_bits; ≥2 more fixed
seeds + owner DNS seed; final U==U′ capture AT H; x86-64 frozen-vector CI
run; the four release gates. Nothing in this pack forecloses or presumes
any of those decisions.
