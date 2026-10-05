# B3 Bridge V2: owner rulings of 2026-10-03 to 10-05

> Record of explicit project-owner decisions given in conversation with
> Claude on 2026-10-03 and 2026-10-04, quoted verbatim. Committed on
> `feat/flowmeshV2-dev-claude` so the decisions are traceable in the
> repository. Under CLAUDE.md's precedence order these are level-1 owner
> rulings. This file does **not** edit CLAUDE.md, the master handoff, the
> architecture contract or `b3-open-decisions.md`; reconciling those
> governing texts is listed below for the owner's review when this branch
> is merged. Nothing here authorizes activation, deployment or a release.

## Rulings

| # | Ruling | Owner's words |
|---|---|---|
| R1 | Deposits and withdrawals should take 30-60 minutes, not days | "people should not wait for days in order to deposit or withdraw" ... "well 30 mintues to 1H is okay" |
| R2 | No USDC in any form | "circle will never mint on our chain for now for sure , so leave it"; "they need full control , which is not possible in fully decentralised system" |
| R3 | Fresh start: new contracts; the old Ethereum stack (vault `0x0778…c64b`, verifier `0xE72B…F690`, prover `0x8e61…dA7`) may expire | "eth usdt will never gonna work untill we resolve conflicts, now on arb it will be new" ... "you should do it ... I think thats the best approch" |
| R4 | Everything must work end to end on regtest first | "I need everthing to be working atleast on regtest" |
| R5 | Launch with the simpler plan, without a liquidity provider: one fresh Ethereum vault (USDT) backs bUSD 1:1; Arbitrum users go through USDT0's LayerZero bridge; no Arbitrum vault, no signer-attested minting at launch | "ok then I think simpler plan , and its without LP ?" |
| R6 | As decentralized as possible; signers, if any, rotate | "to make it fully decentralise d"; "what I mean signers can be rotated" |
| R7 | An overcollateralized coin, if built, is a separate product with another name | "we can also make it but we will call it something else not bUSD" |
| R8 | Proceed with stage S3 (fresh finality lineage, contracts V2, node core slice, regtest RT4) on Claude's own branch; this explicitly overrides CLAUDE.md rule 3 ("Modern PoS ... do not replace or redesign it") and the no-consensus-change line (`CLAUDE.md:12-13`) for that work and for the H1/H2 chain-health fix | "go S3" (answering: "Reply go S3 and I'll record your decision in a commit and start building" after "It changes B3's finality rules, so I need your OK to override CLAUDE.md rule 3. That OK also covers the H1 producer fix.") |
| R9 | (2026-10-05) The K_OF_N bootstrap attestation must bind the Verifier V2 contract address and the EVM chain id, so a bootstrap signature cannot be replayed on another deployment or chain (open issue OI-12) | "yes OI-12" (answering: "bind the 4-key start message to the contract and chain (OI-12); I recommend yes") |

Earlier standing authorization (owner memory, 2026-09-30/10-03): commit to
Claude's own branch `feat/flowmeshV2-dev-claude`; never push; the owner
merges.

## Scope of R8

- Allowed on this branch: implementing and testing, on regtest and in unit
  tests, the fresh lineage and digest V2, Verifier V2 / Vault V2 /
  BLS digest prover / relay batch contracts, the bridge V2 node regime for
  one Ethereum vault, and H1/H2, as specified in the `b3-bridge-v2-*.md`
  proposals.
- Not allowed: changing mainnet or testnet behaviour in a way that could
  activate on a live chain, setting mainnet activation heights, deploying
  contracts, sending transactions, pushing, or tagging a release.
- OD-8's signer-set ban stays in force (R5).

## Governing text to reconcile before any activation

Proposals, not edits made here (details: architecture §16 and §20):

1. `CLAUDE.md` rules 3, 5 and 6, the workspace lock and `:12-13`.
2. `doc/design/b3-open-decisions.md` OD-8 (single-USDT wording; bUSD V2's
   new vault-bound AssetId; the old vault's expiry).
3. `doc/design/b3-master-handoff.md` and `b3-architecture-contract.md`
   bridge sections (deployed verifier unchanged; 3-of-4 bootstrap wording;
   W unset vs code's 811,001).
4. V2 Stage 0 R rows that keep the deployed Ethereum verifier unchanged.
5. The 2026-09-30 stable-quote ruling, recorded only in owner memory.
