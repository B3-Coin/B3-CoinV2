# B3 Hive documentation

## Setup

B3 Hive is the desktop wallet and full node for the B3 FlowMesh network. It
downloads and validates the B3 chain. Synchronization time depends on the
computer, storage, network connection, and available peers. Wait for the wallet
to report that synchronization is complete before spending.

Release binaries should be obtained from B3's official release channel and
verified against the published checksums and signatures.

## Running

- Unix: run `bin/b3coin-qt` for the desktop wallet or `bin/b3coind` for a
  headless node.
- Windows: launch **B3 Hive** from the Start menu or run `b3coin-qt.exe`.
- macOS: move **B3 Hive.app** to Applications and open it there.

## Building

- [Dependencies](dependencies.md)
- [macOS build notes](build-osx.md)
- [Unix build notes](build-unix.md)
- [Windows build notes](build-windows-msvc.md)
- [FreeBSD build notes](build-freebsd.md)
- [OpenBSD build notes](build-openbsd.md)
- [NetBSD build notes](build-netbsd.md)

## Development

The repository's [root README](../README.md) describes the development process
and automated testing.

- [Developer notes](developer-notes.md)
- [Productivity notes](productivity.md)
- [Release process](release-process.md)
- [Translation process](translation_process.md)
- [Translation strings policy](translation_strings_policy.md)
- [JSON-RPC interface](JSON-RPC-interface.md)
- [Unauthenticated REST interface](REST-interface.md)
- [BIPs](bips.md)
- [DNS seed policy](dnsseed-policy.md)
- [Benchmarking](benchmarking.md)
- [Internal design documents](design/)

## Operations and wallet guides

- [Assets attribution](assets-attribution.md)
- [`b3coin.conf` configuration](bitcoin-conf.md)
- [CJDNS support](cjdns.md)
- [Files and data directories](files.md)
- [Fuzz testing](fuzzing.md)
- [I2P support](i2p.md)
- [Init scripts](init.md)
- [Managing wallets](managing-wallets.md)
- [Multisig tutorial](multisig-tutorial.md)
- [Offline signing tutorial](offline-signing-tutorial.md)
- [P2P bad ports](p2p-bad-ports.md)
- [PSBT support](psbt.md)
- [Reduce memory use](reduce-memory.md)
- [Reduce network traffic](reduce-traffic.md)
- [Tor support](tor.md)
- [Transaction relay policy](policy/README.md)
- [ZMQ](zmq.md)

Source code and issue tracker: <https://github.com/B3-Coin/B3-CoinV2>

## License

B3 Hive is distributed under the [MIT software license](../COPYING) and retains
the attribution notices of its upstream projects.
