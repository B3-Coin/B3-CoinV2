# B3 Hive

B3 Hive is the desktop wallet and full-node software for the B3 FlowMesh
network. It downloads and validates the B3 chain locally, manages
user-controlled keys, and provides the foundation for B3's modern protocol
components.

Further information is available in the [doc folder](doc/).

## Programs

- `b3coin-qt` — B3 Hive desktop wallet
- `b3coind` — headless full node
- `b3coin-cli` — RPC command-line client
- `b3coin-wallet` — offline wallet utility

## License

B3 Hive is released under the terms of the MIT license. See
[COPYING](COPYING) for more information. The source retains the copyright and
attribution notices of Bitcoin Core and other upstream projects from which it
derives.

## Development process

Development branches are built and tested regularly, but are not guaranteed to
be stable. Release branches and signed tags identify reviewed releases.

The contribution workflow is described in
[CONTRIBUTING.md](CONTRIBUTING.md), and useful guidance is available in
[doc/developer-notes.md](doc/developer-notes.md).

## Testing

Testing and code review are essential for security-sensitive software.
Developers are strongly encouraged to add [unit tests](src/test/README.md) for
new behavior and regressions. Unit tests can be compiled and run with `ctest`.

Regression and integration tests live under [test](test/) and can be run with
`build/test/functional/test_runner.py` when their dependencies are installed.
Release changes must be tested on all supported platforms.

### Manual quality assurance

Changes should be tested by somebody other than the developer who wrote the
code, especially when they affect wallet storage, transaction encoding,
consensus boundaries, or release packaging. A clear test plan should accompany
changes that are not straightforward to verify.

## Translations

Translation sources and inherited locale files live under `src/qt/locale`.
See the [translation process](doc/translation_process.md) and
[translation strings policy](doc/translation_strings_policy.md) before making
translation changes.
