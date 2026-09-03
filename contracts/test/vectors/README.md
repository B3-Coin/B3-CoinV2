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
