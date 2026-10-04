# B3 Hive

## Experimental FlowMesh tester candidate

This branch builds **1.1.5-flowmesh-test.2**, not a production recovery release.
The opt-in preliminary-agreement mode still has a reproduced withdrawal
certification stall. Existing locked markets are not repaired. Use generated
wallets and valueless coins on an isolated test network; do not upgrade a live
signer or reset its history to test this candidate.

Read [FLOWMESH-TESTING.md](FLOWMESH-TESTING.md) for the known failure, connection
commands, operator/client distinction and qualification limits. Build artifacts
and passing unit tests are not end-to-end certification or deployment approval.

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
- `b3-bridge-ethcheck` — optional bridge proof verifier and canonical payload
  emitter (`-DB3_BRIDGE_TOOLS=ON`)
- `b3-bridge-bootstrap-proof` — optional offline 3-of-4 bootstrap proof and
  Ethereum initialization-calldata builder (`-DB3_BRIDGE_TOOLS=ON`)

Linux operator archives also include the automatic Ethereum-to-B3 relayer and
its fail-closed operating guide under `contrib/b3bridge/`. Bridge tools being
present in an archive does not activate an unconfigured mainnet bridge.

## License

B3 Hive is released under the terms of the MIT license. See
[COPYING](COPYING) for more information. The source retains the copyright and
attribution notices of Bitcoin Core and other upstream projects from which it
derives.

## Development process

Development branches are built and tested regularly, but are not guaranteed to
be stable. Release branches and version tags identify reviewed releases. Check
each release's notes for its exact signature and checksum guarantees.

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

## Student assistant prototype

The Hive sidebar now includes **Assistant**, with Chat, Lessons and Connection
tabs. This is a separate application feature; it does not change B3 consensus.

In **Connection**, enter a model-service URL, a model name and, when required,
an API key. The service must support the Chat Completions request format and
function calling to use node tools. HTTPS is required for remote services; HTTP
is accepted only for literal loopback addresses. Click **Apply connection**,
then send a message in Chat. Applying settings alone sends no request. No model
is bundled, downloaded, started or trained by this prototype.

Chat and API keys stay in memory. Only the service URL and model name are saved
in application settings. Messages and relevant approved lessons are sent to the
configured service. Node tools are off on every startup and require explicit
opt-in. Their complete allowlist is `getblockchaininfo`, `getnetworkinfo`,
`getfinalitystatus` and `listflowmeshmarkets`, without arguments. An explicit
field projection removes wallet accounts/balances, proxy and address details,
market endpoints, local queued-action counts, signature pools and raw RPC error
text before results reach the model or the evidence view. Market listing still
uses B3's existing wallet RPC and may be unavailable without an unambiguous
loaded wallet. The assistant cannot create or sign transactions, place trades,
execute shell commands, or call other RPCs.

Lessons starts with four source-cited B3 lessons pinned to the inspected
`30825f9` baseline. A teacher can correct an answer, identify its source, save a
draft and explicitly approve it. Only approved lessons are retrieved into chat.
Custom lessons live in `student-ai/lessons.json` under Qt's application-data
location; writes are atomic and detect conflicting or damaged files. Their
SHA-256 identifiers identify exact lesson content, not its truth. Approval is a
review decision. This is local retrieval memory, not training of model weights,
an autonomous teacher service or an on-chain learning commitment. Existing B3
block anchors refer to chain positions and are not lesson storage.

Build with Qt GUI tests enabled, then run the isolated assistant checks:

```sh
cmake --build build --target test_b3_ai-client-qt test_b3_ai-lessons-qt test_b3_ai-public-data-qt test_b3_ai-page-qt test_b3_ai-shell-qt b3_student_ai_preview
ctest --test-dir build -R '^test_b3_ai-' --output-on-failure
```

The model-client tests use a loopback mock service; the other checks create no
node, wallet, model connection or chain datadir. The manual preview at
`build/bin/b3_student_ai_preview` uses disposable settings and lessons, without
starting B3. Pass `--capture-dir /path/to/screenshots` to capture its four UI
views. A preview is disconnected until a service is configured; it does not
simulate successful model replies or invent node observations.

Next development milestones are a reproducible B3 question/task evaluation set,
teacher-reviewed improvement experiments and then additional task tools with
appropriate confirmation. Model training and verifiable publication of lesson
revisions require separate designs and measured results.
