# B3 Hive v1.1.1-beta.3 — Modern-PoS Transition Beta

This prerelease replaces v1.1.1-beta.2 for validators preparing for the
block-811,000 Set0 snapshot and the first Modern-PoS block at 811,001.

## Included corrections

- Post-boundary B3 peers advertise protocol 80009 while remaining compatible
  with historical 80008 peers and pre-beta.3 modern peers. Core feature
  negotiation remains capped at 70016; this does not change consensus.
- The raw-transaction fee-rate safety guard now uses the modern B3 unit. Its
  inherited legacy-unit value could reject correctly signed finality-key
  transactions during manual rebroadcast even though their real fee was below
  the wallet's safety limit.
- Operator guidance now states the implemented dual finality threshold
  accurately: with three validators all three must sign; with four validators,
  three signatures are sufficient only when they also carry more than
  two-thirds of total stake weight.

## Validator deadline

A stake included in block `b` becomes active at `b + 20`. To enter Set0, each
validator's stake must therefore be included no later than block 810,980, its
finality-key binding must be confirmed and non-revoked, and `startstaking` must
be running before block 811,001. Producing a Modern-PoS block does not spend or
reset the stake deposit.

## If a wallet stops at block 810,000

Install this transition build first. A pre-transition binary intentionally
cannot accept block 810,001 because it does not contain the sealed block-810,000
anchor. A current node also needs at least one post-transition peer; protocol
80008-only peers belong to the retired legacy network.

Do not reindex by default. The block-index startup problem from beta.1 was
already corrected in beta.2. Use one full `-reindex` only when startup explicitly
reports an incompatible old block index; `-reindex-chainstate` is not sufficient
for that specific case.

## Bridge status

The decentralized Ethereum bridge remains fail-closed in this build. Three
trusted B3 validators are enough to start Modern PoS, but the bridge requires
four complete public bootstrap identities and both its current and successor
canonical B3 sets must independently satisfy the four-validator and weight
floors. This prerelease does not enable deposits or withdrawals and must not be
advertised as an active bridge release.

This is an unsigned prerelease. The complete transition and bridge changes are
documented in `doc/release-notes-v1.1.1.md`.
