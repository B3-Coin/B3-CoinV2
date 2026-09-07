# Fast, offline finality recovery checks

Configure the normal project with `BUILD_TESTS=ON`, then build these targets:

```sh
cmake --build build --target test_operator_recovery_signer test_finality_recovery_options test_targeted_recovery_store -j4
ctest --test-dir build --output-on-failure -R '^test_(operator_recovery_signer|finality_recovery_options|targeted_recovery_store)$'
```

They link the same freshly built production libraries as the wallet. Do not
mix old node/common/consensus objects with new headers: the recovery container
changes the internal `Consensus::Params` layout, not the disk or wire formats.

- `test_operator_recovery_signer`: actual BLS signer, pool and durable journal,
  with randomized identities and indexed synthetic headers. Covers default-off
  recovery, exact incident and target, 19/20-depth boundary, stronger consensus
  depth, retained original vote, forward-only lock, restart, later valid vote,
  changed/expired lineage, invalid replacement and a second anchor reorg.
- `test_finality_recovery_options`: strict public manifest parser, exact hash
  acknowledgement, integer/hex/byte-order checks, paired startup options,
  regular files, size limits, and unchanged output on refusal.
- `test_targeted_recovery_store`: production atomic journal writes, exact
  target/incident checks, stale writers, preserved original vote and restart.

These programs accept no datadir or wallet arguments and only create/remove
their own unpredictable temporary directories. They do not start a node,
mine a legacy prefix, connect to peers, access a live wallet or approve a real
incident. The signer fixture supplies tracker state: it is not a test of
consensus derivation, mixed-old/new networking, Ethereum execution or safe
selection of a real recovery anchor. Old signatures remain valid under V1;
20-block depth alone does not prove finality.

Additional focused core suites cover wallet RPC authorization, actual stopped
staking controls, snapshot key selection, pool budgets and read-only status.
See the versioned release qualification record for exact selectors and results.
