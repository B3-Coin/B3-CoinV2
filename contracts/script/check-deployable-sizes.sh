#!/usr/bin/env bash
# Check only contracts that are actually created on Ethereum. `forge build
# --sizes` also counts Foundry deployment scripts; those are local tooling and
# may exceed EIP-170 without affecting any deployed runtime.
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
cd "$script_dir/.."

limit=24576
contracts=(
  src/BlsCertificateProver.sol:BlsCertificateProver
  src/B3FinalityVerifier.sol:B3FinalityVerifier
  src/B3StakerBridge.sol:B3StakerBridge
)

for contract in "${contracts[@]}"; do
  bytecode=$(forge inspect "$contract" deployedBytecode)
  bytecode=${bytecode#0x}
  test -n "$bytecode"
  test $((${#bytecode} % 2)) -eq 0
  size=$((${#bytecode} / 2))
  printf '%s: %s bytes\n' "$contract" "$size"
  if ((size > limit)); then
    printf '%s exceeds the EIP-170 runtime limit of %s bytes\n' \
      "$contract" "$limit" >&2
    exit 1
  fi
done
