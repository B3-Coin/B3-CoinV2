# FINALIZED — the owner's ledger of final state

Advanced through the owner's 2026-09-01 FN/asset/FlowMesh activation rulings. This
branch records what is FINAL; open items live in
doc/design/b3-open-decisions.md. Earlier FN claim/proof designs remain history,
not competing activation specifications.

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
  0x1f008000; **M = 811,001**). **X =
  2413ba59476afb9a01b971c350b2c5a51494b37925055be42dde774f30d865c6**
  is pinned. The sealed UTXO set has 1,241,032 outputs; its RPC
  `hash_serialized_3` is
  `9873ede9664a588594e0befdabc8e9cc9267159cfeda5439205a25436d577764`,
  while the independent three-way canonical-row commitment is
  `5e7011beee57f403be78ce66c983218e1ee5c934d60dc4b43503d2d6a70760c7`.
- Modern PoS V1 + BLS finality: implemented, multi-node soak-qualified,
  persisted fail-closed pin; F = M. Full mainnet replay from genesis
  against live peers PASSED (height 808,751, zero errors); real-history
  U==U′ campaign PASSED.
- The transition consensus pins are present on this release branch. The
  separately gated bUSD bridge remains fail-closed on mainnet.
- Transition release contract: pin X, measured S_H-derived R0, and the complete
  independently reproduced FN rights manifest/count/Merkle root during the
  seal pause. Block 810,001 is mandatory FN Genesis: its corridor coinbase
  creates one amount-1 FN output per manifest row in canonical order. The
  outputs use ordinary 30-block coinbase maturity and no extra transfer lock.

## Economics (OD-2 CLOSED 2026-08-26)
- Emission: R0 = floor(S_H × 1% / 525,600) at M, halving every 525,600
  blocks (one year); corridor = 0 + fees; split 90% producer / 10%
  treasury enforced in the coinbase; verified live on a rehearsal chain.
- Treasury: ONE wallet, `SNyANHiUkuqPSfbeKHDXzVD86LC2ZUUjLX` (pinned).
  Simple-v1 colored-asset issuance pays a flat 1,000 B3 to it. FlowMesh's
  spot fee is 0.01% of matched native-B3 notional, split 80/20 between the
  active FN seats and treasury. After each ordinary slot, treasury settlement
  flushes the deterministic maximum currently supportable amount:
  `min(accrued treasury available, anchored native capacity - existing pending
  native withdrawals)`, but only when positive. It never waits for the full
  balance, and zero capacity never blocks trading.
- Denomination: 1 B3 = 1e9 base units, the ONLY human-facing unit
  (RPC, GUI, config); typed amount = staking weight.
- FN Coin: one global zero-decimal asset, lifetime cap 5,000. The final
  through-H manifest count R is measured and independently reproduced before
  the transition tag; all R units are issued directly to their historical
  legacy P2PKH owners in the 810,001 coinbase, with no claim, holder proof,
  deadline, or fee. Modern capacity is exactly `5,000 - R`; modern PoD cost is
  15,000 / 30,000 / 60,000 B3 across successive 500-unit tiers. Owner-script
  signatures authorize transfers after ordinary coinbase maturity; native B3
  pays their network fees. Permissionless modern PoD creation remains
  fail-closed until the separately pinned post-M height A1.
- Colored assets: simple-v1 only — one genesis mints the full fixed supply,
  no later mint path, chain-bound AssetId, and 1,000 B3 issuance fee to the
  treasury. The transition release pins post-M height A2 and remains
  fail-closed before it.

## Planned feature releases (owner cap: two)
- **Transition release:** complete FN + simple-v1 asset consensus paths, final
  seal pins, mandatory real-history shadow-fork rehearsal, FN Genesis at
  810,001, ordinary-maturity FN transfers, later A1 modern PoD creation, A2
  colored assets and FN-seat pre-binding, then A3 FlowMesh spot trading after
  at least the 30-block preparation runway. For each market, the unique epoch-0
  anchor is the earliest canonical block at or after `market.created_height`
  whose post-block FN-v2 seat set has at least four members; sequence zero may
  begin only when that exact anchor is 30 blocks deep. The first approved
  dollar market, once all independent bridge gates pass, is the explicitly
  registered bridge-backed bUSD asset against native B3. Public pause estimate:
  approximately 2–4 weeks.
- **Second feature release:** reserved for later FlowMesh expansion after a
  dedicated testnet with real FN holders and an honest speed benchmark; the
  working spot FlowMesh product itself ships in the transition release.
  Emergency security/correctness releases remain possible and are not
  feature-plan expansion.

## Bridge
- The production flow is deploy-then-pin. Deploy the exact independently
  audited finality verifier and immutable USDT vault, then collect from the
  mined deployment: Ethereum chain ID; verifier and vault addresses; both
  runtime code hashes; the vault deployment block; canonical USDT token
  address; and the vault-derived B3 `AssetId`. A later reviewed B3 build must
  pin that complete tuple. No address, ticker, document, deployment, or partial
  parameter set activates the bridge.
- The bridge fails closed until every pin is present and matches the audited
  deployment and the remaining checkpoint, fork, cap, adapter, rules, proof,
  and operating gates. The vault may be deployed before M, but deposits remain
  disabled until the verifier has been initialized and a fresh valid
  certificate demonstrates qualified current and successor validator sets.
  There are no corridor deposits and no pre-readiness custody window.
- Inbound bridge activation `B` and irreversible-burn activation `W` are
  separate consensus pins. `W` must be at least `B`; unset `W` rejects every
  managed or decentralized withdrawal record without changing the bridge
  AssetId or registry id. From `B`, finality certificates still commit the
  canonical nonzero empty/current withdrawal-tree root so Ethereum readiness
  and verified deposits can operate.
- If `B` is enabled while `W` is unset, depositors receive bUSD but knowingly
  enter a custodial waiting period: they cannot burn it for Ethereum release.
  Ethereum withdrawals and B3 bridge burns remain disabled unless and until
  canonicality and liveness safety for the complete round trip are actually
  solved, independently reviewed, rehearsed, and explicitly enabled in a later
  B3 build by pinning `W`. A deployed verifier or a fresh certificate is not by itself
  authority to burn bUSD or release USDT.
- The historical managed smoke vault
  `0x143F207e23e6aebD7E974be90ac6D434f4c7BFb6` and its owner-controlled
  release path are excluded from the production registry. Its observations
  remain historical evidence only and are not production bridge pins.

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

## Remaining before publication
X, S_H, integer R0, the independently reproduced FN manifest/count/root,
A1 = 812,000, A2 = 813,000, A3 = 815,000, and final-H three-way equivalence
are pinned on this branch. Remaining release operations are the isolated
real-history shadow-fork rehearsal through the activation schedule, release
signing keys, and a green five-platform Actions release. FN Genesis,
owner-script authorization, modern PoD, simple-v1 asset issuance, the treasury
fee, and FlowMesh spot engine/custody/P2P/wallet paths are implemented behind
those exact heights. Bridge-backed bUSD minting remains fail-closed until its
separate bridge gates are closed.
