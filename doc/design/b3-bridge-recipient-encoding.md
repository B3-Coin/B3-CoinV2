# b3Recipient encoding — PROPOSAL for owner ruling

The vault's `Deposit(uint64 indexed depositId, address indexed token,
uint256 amount, bytes32 b3Recipient)` event carries an opaque 32-byte
recipient. Stage-4 consensus must define its meaning before ANY public
deposit UI exists (threat model / mint-rules section 5; a UI against an
undefined encoding strands funds semantically).

## Proposed encoding V1 (RECIPIENT_V1)

```
b3Recipient = 0x00 x 11 ‖ 0x01 ‖ HASH160(recipient pubkey)   (32 bytes)
                 padding    tag       20-byte P2PKH hash
```

- Byte 11 is a one-byte VERSION TAG (0x01 = P2PKH hash160). Future
  recipient kinds (script hashes, policy outputs) get new tags without a
  contract change; unknown tags are UNMINTABLE until a later ruling
  (funds remain locked, refundable by governance path — never guessed).
- Derivation is exactly the treasury-address primitive: any B3 address
  decodes to its hash160; Hive (or the static page) derives the tag from
  a pasted/owned address with no new cryptography.
- All-zero recipient (the smoke-test value) is explicitly UNMINTABLE.
- Stage-4 mint pays a standard P2PKH output to the embedded hash160 in
  base units after the ruled decimals/raw-unit conversion of the
  registry entry (mint-rules section 5, item 2).

## Rejection rules (consensus, stage 4)

- padding bytes 0..10 not all zero -> unmintable
- version tag != 0x01 -> unmintable (until a later ruling defines it)
- registry inactive for (chainId, vault, token) -> no mint (section 5)

Status: PROPOSED 2026-08-26 — awaiting owner ruling. UI phases
(Etherscan verification, static page, Hive bridge page) are gated on
this ruling and on the v1 release.
