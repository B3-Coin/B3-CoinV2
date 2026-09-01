# B3 Hive v1.1.0 — Transition Release

B3 Hive v1.1.0 resumes the chain after the sealed legacy height and carries
the consensus rules for the temporary PoW corridor, Modern PoS, historical FN
Genesis, permissionless modern FN creation, simple-v1 colored assets, and
FlowMesh v1 spot trading. Every post-transition feature remains fail-closed
until its separately pinned activation height and required mainnet constants
are present.

## Important activation sequence

- Legacy B3 ends at height 810,000. The release pins that block's exact hash
  (`2413ba59476afb9a01b971c350b2c5a51494b37925055be42dde774f30d865c6`)
  and the supply-derived Modern PoS initial reward (19,836,712,254 base
  units).
- Block 810,001 starts the 1,000-block modern-format PoW corridor and must
  include the complete historical FN Genesis manifest in its coinbase.
- Historical FN units go directly to their legacy P2PKH owner scripts. There
  is no claim transaction, proof, deadline, or issuance fee. Ordinary
  30-block coinbase maturity applies.
- Modern PoS starts at height 811,001.
- Permissionless modern FN PoD activates at A1 = 812,000.
- Simple-v1 colored-asset issuance and FlowMesh seat/vault preparation activate
  at A2 = 813,000.
- FlowMesh trading, checkpoints, and vault settlement activate at A3 = 815,000,
  after a 2,000-block preparation runway. Each market's epoch-zero anchor is the earliest canonical
  block at or after `market.created_height` whose post-block FN seat set has at
  least four seats; sequence zero waits until that exact block is 30-deep.

## FN and asset behavior

- FN is one indivisible asset capped at 5,000 units. Modern PoD destroys
  native B3 at the ruled 15,000 / 30,000 / 60,000 B3 tier schedule.
- Simple-v1 asset genesis creates the full fixed supply exactly once and pays
  1,000 native B3 to the treasury. No later mint operation exists.
- FN and colored assets move under the recipient's ordinary owner script;
  native B3 pays transaction fees.
- Typed asset outputs use the modern B3A1 Policy Output envelope and do not use
  `OP_RETURN`. Burns use the unspendable typed BURN policy.

Wallet RPCs include `getassetstate`, `getwalletassets`, `issueasset`,
`sendasset`, `burnasset`, and `createfncoin`.

## FlowMesh v1

- FN holders bind seats during the A2 preparation window. At least four active
  seats are required for a market; otherwise that market pauses safely without
  stopping the B3 chain.
- A changed seat set takes control only after the outgoing set's exact handoff
  checkpoint is itself 30 blocks deep. A shallow reorg can therefore republish
  the handoff without stranding either committee.
- FlowMesh uses its dedicated authenticated peer messages for fast microblocks.
  DEX vault UTXOs are typed B3A1 Policy Outputs; checkpoints and vault proofs
  are typed MPA records. Neither uses `OP_RETURN`.
- Spot trades charge the ruled 0.01% native-B3 fee, split 80% equally across
  active FN seats and 20% to treasury. Native B3 pays B3 transaction fees;
  FlowMesh can trade B3 against registered colored assets.
- A validator returning after a long outage checkpoints certified withdrawal
  effects in deterministic chunks of at most 4,096, split only at B3 block
  boundaries. This is certified-log batching, not base-chain transaction
  batching; the withdrawal publisher remains strictly sequential.
- Withdrawal requests are admitted only while pending obligations fit the
  deterministic capacity of the largest 64 live pool UTXOs. Payouts select
  amount-descending UTXOs, then outpoint for ties. The publisher sends one
  withdrawal, waits for confirmation, refreshes capacity, rebuilds, and only
  then sends the next.
- After each ordinary slot, treasury fees flush by the largest positive amount
  that fits anchored native capacity after existing pending native withdrawals.
  A partial flush is valid; zero capacity never blocks trading.
- Before signing, a validator atomically stores the exact candidate and its
  authenticated action evidence with the permanent safety lock. After a
  restart it can revalidate and resume that same candidate without risking a
  second vote or waiting for users to resubmit actions.
- A valid signed proposal ahead of a returning validator's local head starts
  the existing bounded catch-up flow, so an honest three-of-four committee can
  recover even if its other up-to-date member is offline.
- v1 deliberately uses permanent per-sequence signing locks and has no
  view-change protocol. A malicious proposer that splits honest seats can halt
  that FlowMesh market, but cannot create two valid B3 checkpoints or fork B3.

The canonical bUSD identity is Ethereum-mainnet USDT
`0xdAC17F958D2ee523a2206206994597C13D831ec7` held by managed-v1 vault
`0x143F207e23e6aebD7E974be90ac6D434f4c7BFb6`, represented 1:1 in raw
six-decimal units. Managed-v1 withdrawals use the vault's immutable release
authority, but no arbitrary authority payment is a valid redemption. A
confirmed B3 burn request must bind canonical bUSD, the exact raw amount,
Ethereum recipient and unique request id. The exact B3 BURN/request record and
reorg/reindex tracking are implemented; the operator waits the pinned B3
finality depth, releases exactly once, durably consumes the id, and reconciles
reserves against supply. The operator release automation and durable
request-consumption database are not implemented. No confirmed burn means no
release.

bUSD minting remains disabled on mainnet. The current tree has a strict bounded
type-10 bootstrap/update/mint/backfill/managed-withdrawal carrier, exact OWNER
mint and bUSD BURN transitions, light-client/anchor/nullifier/cap state,
undo/reindex replay, and mempool/miner/asset integration. Each record requires
one zero-value policy-9 `BRIDGE_RECORD` metadata output committing its exact
canonical bytes. Managed-withdrawal consensus allows only exact ECDSA
`SIGHASH_ALL` or Schnorr `SIGHASH_DEFAULT`/`SIGHASH_ALL` on every input, so the
withdrawal-recipient commitment is signed without `OP_RETURN` or a custom
sighash; `NONE`, `SINGLE`, and `ANYONECANPAY` are rejected.
State is rebuilt in memory from bridge activation; configured bridge nodes
refuse pruning and snapshots that skip history because no durable sidecar
exists. Activation still requires adapter enforcement, independent
review/audits, reproducible runtime evidence, the operator withdrawal service,
and production checkpoint/fork/cap/approval/activation/rules/X pins.

The deployed vault is immutable. A later decentralized withdrawal design
requires a new audited vault, and the current identity formula makes that a
new `AssetId`. The announced migration must include old-registry cutoff,
late-deposit handling/refunds, burn/swap/reissue of old bUSD, reserve movement
without a duplicate mint claim, and the new vault/verifier/identity pins.

## Upgrade safety

Back up `wallet.dat` and shut down the old client cleanly before upgrading.
The release uses the existing B3 datadir and supports legacy wallet import and
rescan. Verify every downloaded package against the published SHA-256 sums.

The seal packet now contains independently verified X and R0, the byte-identical
3,592-row FN manifest (root
`e8f282a7dcaa9a8fbcfcc5c22ba4f456e5b50968fcf899aaacdaca65bef898ec`,
SHA-256 `c80470eec785600f33fa2e69c520ff331c2b354ebf6e0a9bf8cae7d1eb5f9dca`),
the A1/A2/A3 heights, and the final-H three-way equivalence result. The release
must not be tagged or distributed until the isolated shadow-fork rehearsal also
passes. Any bridge activation additionally requires every bUSD security pin
named above; bUSD remains fail-closed independently of native/colored FlowMesh.
