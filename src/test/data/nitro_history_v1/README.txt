Genuine owned Nitro development-chain history fixture, version 1.

Produced by the official OffchainLabs Nitro v3.12.1 image, AMD64 manifest
sha256:948cf8c42a6ed85bc055f13866fb9a029c8234dc16c12d7ffa4ed669270aaea8,
source commit 70fa99a6e711f09eedd96a907290c3df0f3bcf7d.
The owned standalone dev chain has id 412346; its exact genesis is retained
in source-context.json. No external RPC state or archive snapshot was used.

proof.bin contains raw unchanged headers for blocks 7 and 6, all four raw
transactions/receipts, and account/storage witnesses at both endpoints.
request.json retains the independently supplied previous-height/hash and
to-height/hash request. expected-events.json derives its expected event
records from the original Nitro JSON receipts and independently specified
fixture call arguments; these JSON receipts are expectations, not proof
authority. The binary includes Nitro system transactions as well as the
public development-account signed calls. No private production keys occur
in this fixture.

The deployed NitroWitnessFixture is explicitly an event/storage emitter,
not ERC20 custody. Its two transactions emitted the Rebirth Deposit and
Withdrawn ABI solely to exercise candidate-event decoding. Storage value
changed from 10000000 to 9000000 and internal nonce from 1 to 2; slot 3
is proven absent at both endpoints.

The assertion fields in proof.bin are neutral zeros. This fixture proves
no L1 settlement, sequencer validity, Arbitrum One history, token backing,
operator source authority, or mainnet readiness. A separately supplied L2
checkpoint is not authenticated by the offline history checker.
