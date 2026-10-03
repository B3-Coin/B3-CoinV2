# FlowMesh V2 accounting and restart recovery

Published 4 October 2026 · B3 development diary 01 · Tests reproduced 3 October

This first development diary recaps the isolated accounting, agreement and
disk-backed recovery work behind FlowMesh V2. I am grouping related commits
into milestones so each entry explains a useful result rather than every
intermediate edit.

[Watch the published episode on B3 Flow Mesh](https://youtu.be/h8OF2ygjLiE).

## What the models cover

The accounting model follows generated funds through deposits, shared spot
balances, order reservations, fees and payouts. Balances are keyed by exact asset
identity. Transfers between spot and futures cash need explicit authorization
and synthetic risk permission; this is not a complete futures margin or trading
system. Snapshot restoration replays the recorded inputs, and a separate checker
examines conservation and ownership.

The agreement model exercises delayed and duplicate messages, missing data,
partitions, faulty leaders and late certificates. Its checker includes issued
synthetic signatures that peers never received. Its membership, authentication
and anchor evidence are synthetic.

The storage harness adds actual files, SQLite and separate child processes.
Tests interrupt processes at selected boundaries, reopen stores and check that
obligations survive and committed accounting is applied once. Process-kill and
injected-failure tests do not establish machine-power-loss safety.

## Fresh local reproduction

At revision `b4b1de423a4be3efe852deb3ed851d0bfce156fc`, all **332 tests** in the
four selected suites passed: **113 accounting, 142 agreement, 69 storage and
8 runner checks**. This is a fresh local run, not a new GitHub CI result or the
complete branch qualification campaign. Commands and full synthetic test
receipts accompany this episode and are linked below.

The recorded test run used the local revision above. Its relevant model files,
runner, runner guards and native integration gate are byte-for-byte identical to
public revision `269ebc48370b94be3ed26c180232c22236241257`. The
[recorded source identities](SOURCE-EQUIVALENCE.json) preserve this comparison.
This equivalence covers only the selected isolated suites, not the unpublished
native changes between the two revisions.

To reproduce the same isolated model sources from a clean checkout, with
Python 3.14:

```sh
git checkout --detach 269ebc48370b94be3ed26c180232c22236241257
PYTHONHASHSEED=0 PYTHONDONTWRITEBYTECODE=1 python3.14 -B ci/run_flowmesh_models.py --suite runner
PYTHONHASHSEED=0 PYTHONDONTWRITEBYTECODE=1 python3.14 -B ci/run_flowmesh_models.py --suite accounting
PYTHONHASHSEED=0 PYTHONDONTWRITEBYTECODE=1 python3.14 -B ci/run_flowmesh_models.py --suite agreement
PYTHONHASHSEED=0 PYTHONDONTWRITEBYTECODE=1 python3.14 -B ci/run_flowmesh_models.py --suite storage
```

The episode's reproduction imposed a 300-second timeout on each suite.

## What this does not establish

These results are evidence within the declared model boundaries, not an
independent security audit or production-V2 approval. Native integration,
changing membership, complete futures economics, bridge verification and
integrated network latency remain separate gates. No 200 ms performance claim
is made here.

An internally consistent, coherently rewritten history still requires an
external trusted commitment to detect it. Earlier failing examples remain in
the development record beside the repair tests.

The video includes a brief, cropped **historical Qt regtest capture from
13 September 2026**. The other visuals illustrate source and test evidence;
they are not live V2 trades. Narration uses the existing JoshMAin ElevenLabs
voice. No private wallets, keys or signer journals are included.

## Source and evidence

- [Public source revision for the identical model files](https://github.com/B3-Coin/B3-CoinV2/commit/269ebc48370b94be3ed26c180232c22236241257)
- [Accounting model and provisional profile](https://github.com/B3-Coin/B3-CoinV2/blob/269ebc48370b94be3ed26c180232c22236241257/test/flowmesh_v2_model/README.md)
- [Agreement model and declared assumptions](https://github.com/B3-Coin/B3-CoinV2/blob/269ebc48370b94be3ed26c180232c22236241257/test/flowmesh_v2_agreement/README.md)
- [Disk-backed recovery harness](https://github.com/B3-Coin/B3-CoinV2/blob/269ebc48370b94be3ed26c180232c22236241257/test/flowmesh_v2_storage/README.md)
- [Native integration gate](https://github.com/B3-Coin/B3-CoinV2/blob/269ebc48370b94be3ed26c180232c22236241257/test/flowmesh_v2_fastpath_native/INTEGRATION_GATE.md)
- [This episode's sanitized local test receipts](TEST-RESULTS.json)
- [Runner output](evidence/runner.log), [accounting output](evidence/accounting.log),
  [agreement output](evidence/agreement.log) and [storage output](evidence/storage.log)
- [Narration transcript](transcript.txt) and [English captions](B3-Devlog-01.srt)
- [Document and receipt checksums](SHA256SUMS)

The video is public on B3 Flow Mesh. This written companion and its sanitized
receipts are published in this repository; they have not been deployed to
[draft.b3hive.io](https://draft.b3hive.io). The next diary should follow another
verified milestone or a clearly labelled regtest demonstration.
