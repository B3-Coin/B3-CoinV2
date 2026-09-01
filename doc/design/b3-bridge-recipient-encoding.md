# b3Recipient encoding — RECIPIENT_V1 implementation and release gate

The vault's `Deposit(uint64 indexed depositId, address indexed token,
uint256 amount, bytes32 b3Recipient)` event carries an opaque 32-byte
recipient. The type-10 mint path now gives it the RECIPIENT_V1 meaning below;
the production recipient-version/rules pin must still be frozen before ANY
public deposit UI exists (a UI against an unpinned encoding strands funds
semantically).

## Encoding V1 (RECIPIENT_V1)

```
b3Recipient = 0x00 x 11 ‖ 0x01 ‖ HASH160(recipient pubkey)   (32 bytes)
                 padding    tag       20-byte P2PKH hash
```

- Byte 11 is a one-byte VERSION TAG (0x01 = P2PKH hash160). Future
  recipient kinds (script hashes, policy outputs) get new tags without a
  contract change; unknown tags are UNMINTABLE until a later ruling
  (funds remain locked pending an explicitly governed refund — never guessed).
- Derivation is exactly the treasury-address primitive: any B3 address
  decodes to its hash160; Hive (or the static page) derives the tag from
  a pasted/owned address with no new cryptography.
- All-zero recipient (the smoke-test value) is explicitly UNMINTABLE.
- Consensus mint pays one exact B3A1 OWNER output whose owner script is the
  standard P2PKH script for the embedded hash160, in base units after the
  ruled decimals/raw-unit conversion of the registry entry (mint-rules
  section 5, item 2).

## Rejection rules (consensus type-10 mint)

- padding bytes 0..10 not all zero -> unmintable
- version tag != 0x01 -> unmintable (until a later ruling defines it)
- registry inactive for (chainId, vault, token) -> no mint (section 5)
- missing, duplicate, mismatched, or orphan zero-value policy-9
  `BRIDGE_RECORD` commitment -> invalid transaction; standard `SIGHASH_ALL`
  covers this ordinary output, so the recipient binding needs no `OP_RETURN`
  or custom sighash

Status: the codec and exact OWNER-output check are implemented in bridge
consensus validation. Mainnet's recipient-version/rules commitment remains
unset and the broader bridge readiness gates remain open. UI phases
(Etherscan verification, static page, Hive bridge page) are gated on those
pins, independent review, and the bridge activation release. A public deposit
UI must not appear merely because FlowMesh reaches A3.
