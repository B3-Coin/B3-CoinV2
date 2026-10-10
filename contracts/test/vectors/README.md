# 64-member BLS gas vector

`bls64_proof.bin` is the deterministic worst-case V1 certificate proof at the immutable
bridge-authorizing cap of 64 validators. It uses B3's vendored blst v0.3.17,
64 distinct deterministic PoP-verified keys, exactly 43 signers, and the 21
required absent-member witnesses with ordered depth-13 paths.
`bls64_bootstrap_proof.bin` is the matching real 3-of-4 handoff that installs
this 64-member Set_0, allowing the benchmark to execute the complete production
`submitCertificate` path rather than only calling the prover directly.

After configuring the B3 tree as `build-relayer`, regenerate from the repository
root on macOS with:

```sh
clang++ -std=c++20 -Isrc -Ibuild-relayer/src \
  -Isrc/leveldb/include -Isrc/univalue/include -Ideps/blst/bindings \
  contracts/test/vectors/generate_bls64_proof.cpp \
  build-relayer/lib/libbitcoin_common.a \
  build-relayer/lib/libbitcoin_crypto.a \
  build-relayer/lib/libbitcoin_util.a \
  build-relayer/lib/libbitcoin_consensus.a \
  build-relayer/lib/libb3_blst.a \
  -o /tmp/b3_bridge64_vector
/tmp/b3_bridge64_vector \
  contracts/test/vectors/bls64_proof.bin \
  contracts/test/vectors/bls64_bootstrap_proof.bin
shasum -a 256 \
  contracts/test/vectors/bls64_proof.bin \
  contracts/test/vectors/bls64_bootstrap_proof.bin
```

Expected output facts:

```text
set0_hash=23c41c0c48be76ff3983e6e94c460de4c67584414d3c89f911c336b31a396d6c
set1_hash=484039e5e8fe8a24c83aa52af44e284f22d5a9d12c2f0053c3ce862c72a6a090
set_aggregate_pubkey=936e276e891a36681c10868bd616382743a374877d4e36f1fb508c40122885f81cfb75e0395547819b94e35caff5e76e
set_members_root=68adb90eed00952dd48e44f440f9f4e0d1be569f944757226883ff3c6a461124
bootstrap_set_hash=5188225d2e128c1f51a61f4f6bad92dd8da6566549156bae3ebb81728158bf14
certificate_proof_size=18144
bootstrap_proof_size=1504
certificate_sha256=a77a685dbdc9690877ff34265397a13e4d440a123ea56b6005315b627ae8178e
bootstrap_sha256=a1f5e2e103c52a9f69595ed0cbea72810a2ec6bc9a49e9efac180660c0182870
```

Run the post-Fusaka Osaka/EIP-2537 assertion from `contracts/`:

```sh
forge test --match-contract BlsCertificateGasBoundTest -vvvv
```

The repository pins `evm_version = "osaka"` and Solidity 0.8.35. The measured
complete successful `submitCertificate` call is 5,513,351 gas and its exact
calldata is 18,724 bytes. The call includes verifier checks, the prover,
storage writes, and event emission. The test then conservatively charges every
calldata byte at the EIP-7623 40-gas nonzero rate:

```text
21,000 + 18,724 * 40 + 5,513,351 = 6,283,311 gas
```

That is below EIP-7825's 16,777,216 per-transaction cap. Constructor validation
rejects any configured bridge cap above 64, and `bridgeReady` closes if either
the current set or known successor exceeds the deployment-pinned cap.

# Bridge V2 shared vectors

`bridge_v2_vectors.json` is written by `gen_v2_vectors.sh` (python3 stdlib
only; `--check` exits 1 unless the committed file is current) and read by
`contracts/test/v2/VectorsV2.t.sol`, `contracts/test/v2/StorageLayoutV2.t.sol`
and `contracts/test/v2/ForwarderVectorsV2.t.sol`;
B3's C++ bridge tests copy its values. Besides lineage ids, SetHeaders, digest V2, history
and withdrawal trees and the bUSD V2 AssetId, it holds
`bootstrap_attestation_v2` (the 301-byte OI-12 K_OF_N bootstrap message and
its digest for two stand-in deployments), `storage_layout_v2` (the
verifier, vault and Tether slots, bit offsets, example words and mapping keys
that B3 proves by EIP-1186) and `forwarder_v2` (below).

## Forwarder vectors (`forwarder_v2`)

Contracts spec §7.2 and §8; node changes §7.2 and §11.2. All hex is EVM byte
order. Factories, vaults, RECIPIENT_V1 key hashes and EOAs other than anvil
dev accounts 0 (`0xf39f…2266`) and 1 (`0x7099…79c8`) are stand-ins, each
`keccak256("B3-VECTORS/forwarder/<label>")[12:]`, so byte-order mistakes
show. `init_code_hash` is the stand-in `keccak256("B3-VECTORS/forwarder-init-code")`;
the real `FORWARDER_INIT_CODE_HASH` commits to `B3Forwarder`'s metadata and
comes from the build (DeployV2 manifest).

| Key | Content |
|---|---|
| `tags` | `keccak256("B3/FWD/DEPOSIT/V2")`, `keccak256("B3/FWD/EXIT/OFT/V2")` |
| `pinned_dst_eid` | 30110 (Arbitrum), the eid `exit[].is_valid` is computed for |
| `deposit[]` | F_d: `factory`, `vault`, `b3_recipient`, `refund_eoa`, `fee_cap` (one is 2^64 − 1), `not_after` (1 and 2^64 − 1 included), the 192-byte `salt_preimage` = `abi.encode(TAG, vault, b3Recipient, refundEOA, uint96 feeCap, uint64 notAfter)`, `salt`, `account` = `keccak256(0xff ‖ factory ‖ salt ‖ init_code_hash)[12:]` |
| `exit[]` | F_x: `factory`, `dst_eid` (30110 or 30101), `to` word, `max_bridge_fee_bps` (0, 25, 50), `fee_cap`, `refund_eoa`, `is_valid` (isValid without EOA-ness), `salt_preimage` = `abi.encode(TAG, uint32 dstEid, to, uint16 bps, uint96 feeCap, refundEOA)`, `salt`, `account` |
| `refund_eoa_message[]` | regtest and mainnet chain domains: the exact `message` (`B3 bridge V2 refund EOA\nchain domain: 0x<raw domain>\nrefund EOA: 0x<lowercase address>`), its `eip191_digest` (`keccak256("\x19Ethereum Signed Message:\n" ‖ decimal length ‖ message)`) and a 65-byte `signature` r ‖ s ‖ v (v 27/28, low s) by the refund EOA |
| `burn_exit_block[]` | for `exit[exit_case]`: the 67-byte kind 6 exit block `u8 1 ‖ u32 dst_eid ‖ to ‖ u16 bps ‖ u64 fee_cap ‖ refund_eoa` (big-endian) and its `recipient` (that case's F_x account) |
| `deposit_fee_cap[]` | OI-2: `fee_cap` = `fee_per_tx` × 4 (deploy, two flushes, `returnToOwner`; one delivery of at most `MAX_DEPOSIT_RAW` per F_d); the last case is the largest `fee_per_tx` whose cap fits uint64 |

The signatures were made offline with cast 1.7.1 from the public anvil test
mnemonic and are pinned in the generator, which checks each by its own
secp256k1 recovery (it handles no private key):

```sh
cast wallet sign --mnemonic "test test test test test test test test test test test junk" \
    --mnemonic-index 0 "$(printf 'B3 bridge V2 refund EOA\nchain domain: 0x…\nrefund EOA: 0x…')"
```

Changing a message (its domain or EOA) requires re-signing it the same way.
