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

### Training our own model from scratch

`contrib/b3ai/scratch/` is a separate CPU research experiment: our own causal
Transformer, initialized randomly, using NumPy for numerical operations and
manual backpropagation. It loads no pretrained weights. The UTF-8 byte tokenizer
is self-contained. The model includes learned token/position embeddings,
multiple attention heads, residual connections, layer normalization and a feed
forward network. AdamW updates all learned parameters.

The first model is deliberately tiny. The built-in corpus contains 14
source-reviewed training records and 4 held-out records, describing the inspected
B3 baseline. It is a teaching fixture, not enough data for general language
pretraining. Split assignments precede tokenization; random windows stay within
one record. Corpus and split hashes, model configuration, optimizer state, random
generator state and experiment status accompany each checkpoint. These are local
provenance records, with no on-chain commitment or proof of factual correctness.

Use a Python environment that already has NumPy; these commands install nothing:

```sh
python3 -m unittest discover -s contrib/b3ai/scratch/tests -v
python3 -m contrib.b3ai.scratch.train --output build-ai/scratch-experiment --steps 200
python3 -m contrib.b3ai.scratch.sample build-ai/scratch-experiment/model.npz --prompt "B3 is "
```

The training command writes `model.npz`, `corpus.jsonl` and `report.json` to the
chosen output directory. It refuses to replace an existing experiment unless
`--resume` is supplied. Resume restores the learned parameters, AdamW moments and
batch-selection RNG; corpus, model configuration and batch shape must match.
The implementation hash and learning rate must also match. The report records
Python and NumPy versions; identical numerical results assume the same runtime.
The report compares fixed training and held-out byte-prediction losses against
the untrained baseline. A reduction in loss establishes only that this limited
prediction task improved; it does not establish conversational ability or B3
task correctness. Text samples may be incoherent or factually wrong.

`--corpus` accepts the same reviewed-record JSONL schema as the saved fixture.
For custom data, the review flag is the caller's declaration; the trainer does
not independently verify the sources. The report distinguishes custom data from
the exact built-in B3 fixture and records its content hash.

The first local run used 137,216 parameters, a 64-byte context and 200 updates.
Fixed training loss fell from 5.9527 to 2.3886; fixed held-out loss fell from
5.9043 to 2.5298. The generated sample remained incoherent. All 44 numerical,
checkpoint, corpus and adapter tests passed, including exact resumed updates and
custom-corpus provenance. A temporary loopback test with the actual checkpoint
also verified model listing, generated output, tool rejection and context
overflow rejection. This result demonstrates learning on the small fixture,
with no demonstrated chat or task ability.

An optional compatible HTTP adapter is explicitly experimental:

```sh
python3 -m contrib.b3ai.scratch.server --checkpoint build-ai/scratch-experiment/model.npz --allow-experimental
```

It binds only to a loopback IP address and never starts or accesses a B3 node.
It serves generated text as `b3-scratch-experiment`, rejects streaming and tools,
and refuses prompts beyond the checkpoint's context limit. The tiny initial
checkpoint has a short byte context and is not qualified for the normal Hive
assistant prompt. Keep it in the direct sampling/API experiment until a later
model supports the full instruction and lesson context, learned turn endings
and the tool protocol. This byte model has no end-of-turn token: its completions
reach their byte budget and report `finish_reason: length`. No canned response
is substituted for the model's output.

Numerical checks cover finite-difference gradients, causal attention, learning
on a sanity corpus, checkpoint integrity and exact resumed updates. Reliable
chat and tool use remain separate milestones: obtain a sufficiently broad
reviewed language corpus, train dialogue and tool-call behavior, run independent
B3 task evaluations, and then integrate a qualified checkpoint with Hive.

### More training and short dialogue lessons

Continuing the original prose experiment from 200 to 2,000 updates reduced its
fixed training loss from 2.3886 to 0.3437, while fixed validation loss worsened
from 2.5298 to 3.9509. Evaluating all held-out adjacent-byte targets with a sliding
64-byte context confirmed the same trend: 2.5597 to 3.9805. The student was
memorizing the fixture. A separate 400-update checkpoint preserved the best
observed fixed validation loss in this control run, 2.4884. These checkpoints
are retained as distinct experiments.

`dialogue_data.py` supplies teacher-authored short B3 questions and concise,
source-checked answers. Related questions intentionally share consistent answer
targets. Validation contains different questions about those same topics; it
does not establish independent knowledge or broad conversational ability.
Sources and explicit splits are saved with the corpus. The inference function
generates from the learned weights and does not retrieve these target answers.

`teach.py` trains on complete conversation records. Loss applies only to answer
bytes and the final newline; prompt targets and padding are excluded. Validation
scores every validation answer byte and selects the lowest-loss checkpoint.
Patience stops the run when repeated evaluations show no improvement. The saved
parameters, AdamW state and batch RNG all correspond to the selected update;
the report separately records attempted updates and the selected update.

To run this stage with an existing random-start B3 checkpoint:

```sh
python3 -m contrib.b3ai.scratch.teach --output build-ai/scratch-dialogue --initialize-from build-ai/scratch-experiment/model.npz --steps 2500 --evaluation-interval 100 --patience 5 --prompt "What is B3?"
```

Choose a new output directory; existing directories are refused. This stage
resets AdamW moments for the new objective and records the parent checkpoint's
hash. It can increase the context while retaining the existing learned
positions and initializing additional positions randomly. The default is
256 bytes. It requires a float32 parent and identical other model settings.
This is a recorded transfer to a new teaching stage, rather than an exact resume
of the preceding experiment. Custom corpora use caller-declared review labels.
The checkpoint's random-initialization provenance describes the origin of our
own model weights; the parent record identifies this stage's learned starting
weights. It does not mean the transferred parameters were reset to random values.

Actual reply sampling stops at the first generated newline, or reports length
when its budget expires. Empty or incorrect output is preserved for evaluation;
no corrected answer is substituted. Checkpoint selection uses validation data,
so separate probes are required to assess replies outside those questions.
This stage remains experimental and has no tool execution or live node data.
The original HTTP adapter accepts only the earlier sanity-pretraining stage.

The local dialogue curriculum contains 192 training turns and 24 validation
turns across 24 topics. The transferred 149,504-parameter model attempted 900
teaching updates and selected update 400: validation answer loss fell from
2.9335 to 2.1024. A separate 495,360-parameter model started from random weights,
attempted 700 updates and selected update 200, with validation answer loss
6.0366 to 2.3683. Its additional capacity did not resolve the language problem.
Neither candidate produced usable replies on the separate question probes.
These results do not support promoting either checkpoint to a conversational
assistant. All 61 code checks passed; those checks verify implementation and
saved-state behavior, not the factual quality of generated answers.

Training checkpoints and the actual teaching corpus are saved locally. Chat
history in the Hive prototype is session memory, and reviewed local lessons
provide persistent reference context. None of these mechanisms automatically
turns a saved conversation into weight training. Further work needs a sufficient
language-learning dataset and independent reply evaluations; repeating the small
B3 fixtures has already demonstrated overfitting.

### Broader language data and local GPU training

`contrib/b3ai/language/` implements a separate accelerated language experiment
without modifying the saved scratch models. `pilot.json` records the design and
limits. The owner approved PyTorch, NumPy and their required dependencies in the
isolated ignored `build-ai/language-runtime/venv`; that limited exception is
recorded in `CLAUDE.md`. Our model architecture and randomly initialized weights
remain our own. PyTorch supplies device operations and automatic differentiation.

The first objective is readable English continuation, before dialogue teaching
or B3 task qualification. The default 5,395,201-parameter model has six layers,
a width of 256, and 2,048 byte-token positions. Its new tokenizer adds an
end-of-document token, ID 256, to the 256 UTF-8 byte values. This exceeds the old
CPU reference limits and uses the separate `b3-language-transformer/v1` checkpoint
contract. The existing scratch adapter cannot serve it. Model size alone does
not establish language or assistant capability.

The original TinyStories TXT files are pinned to dataset revision
`f54c09fd23315a6f9c86f9dc80f725de7d8f9c64`. The dataset is synthetic fiction under
CDLA-Sharing-1.0; it supplies language examples, not reviewed B3 knowledge.
Use complete `<|endoftext|>`-delimited stories, rather than splitting on newline
or using the historical dataset viewer's broken row boundaries.
See the [dataset card](https://huggingface.co/datasets/roneneldan/TinyStories)
and [paper](https://arxiv.org/abs/2305.07759). Small-model story results do not
establish general conversational or tool capability.

The importer is offline by default:

```sh
python3 -m contrib.b3ai.language.corpus --source /path/TinyStories-train.txt --validation-source /path/TinyStories-valid.txt --output build-ai/language-data-local
python3 -m contrib.b3ai.language.reference --output build-ai/language-reference.npz
python3 -m unittest discover -s contrib/b3ai/language/tests -v
```

An explicit `--fetch`, with no local-source arguments, imports only the two
pinned official HTTPS files. Defaults bound the entire import to 32 MiB of source
bytes and 20,000 documents, with 90/10 budgets for the train and official
validation sources. Each run reserves a new output directory and refuses to
overwrite another run. The official train source stays in train; normalized
content hashes divide the official validation source into validation and test.
Global exact and normalized document deduplication precedes tokenization. This
does not detect all semantically similar stories.

Prepared JSONL files, split checksums, source byte offsets, attribution and a
manifest are saved. Bounded prefixes record their actual prefix checksums and
leave the expected whole-file checksum unverified. Local source identity is
caller-declared unless a complete file matches the published upstream checksum.
Incomplete final stories are excluded; invalid UTF-8 and NUL bytes are rejected.
A prefix pilot is not a representative random sample of the complete dataset.

The local preparation imported 18,000 training stories, 949 validation stories
and 1,051 test stories. Training text contains 16,087,093 UTF-8 bytes. This is
data preparation, not a new trained model or a quality result.

`reference.py` saves initial parameters, logits, masked loss, all gradients,
causal comparison logits, and one clipped AdamW update with its moments. It uses
the existing NumPy implementation and records its implementation hash. The NPZ
contains numeric arrays and JSON bytes, requires no pickle, and is published
atomically without overwriting an existing path. The CPU and MPS model ports pass
the fixed numerical tolerances for logits, loss, gradients and an AdamW update
using identical saved gradients. A complete CPU autograd/AdamW update differs on
two redundant attention key-bias entries because floating-point residuals are
amplified by AdamW's epsilon. Common key bias cancels in attention softmax;
every other parameter and the resulting predictions pass the original
tolerances. Regression tests and the local numerical report explicitly preserve
this distinction; no gradients or tolerances are silently altered.

`batches.py` verifies prepared manifests, source contracts, split hashes and
global normalized uniqueness before loading UTF-8 bytes plus EOD. Batches select
windows within one document. Padding and targets after EOD have zero loss;
the document-ending target is supervised. Validation uses fixed batches and
token-weighted cross entropy; the test split is never batched for selection.

Run with the approved isolated runtime from this checkout:

```sh
build-ai/language-runtime/venv/bin/python -m unittest discover -s contrib/b3ai/language/tests -v
build-ai/language-runtime/venv/bin/python -m contrib.b3ai.language.train --corpus build-ai/language-preparation/tinystories-pilot --output build-ai/language-runs/new-benchmark --device mps --benchmark
build-ai/language-runtime/venv/bin/python -m contrib.b3ai.language.train --corpus build-ai/language-preparation/tinystories-pilot --output build-ai/language-runs/new-pilot --device mps --steps 1000
build-ai/language-runtime/venv/bin/python -m contrib.b3ai.language.train --corpus build-ai/language-preparation/tinystories-pilot --output build-ai/language-runs/new-continuation --initialize-from build-ai/language-runs/new-pilot/model.npz --device mps --steps 1000
build-ai/language-runtime/venv/bin/python -m contrib.b3ai.language.evaluate --checkpoint build-ai/language-runs/new-pilot/model.npz --output build-ai/language-runs/new-pilot/exam.json --device mps
```

Runs reserve fresh output directories. Resource caps apply between complete
operations: at most 1,000 optimizer updates, 8,192,000 supervised training tokens,
7,200 seconds, and 32 GiB process RSS or GPU driver allocations. The initial
random baseline participates in validation selection. `model.npz` retains the
best validation state; `last.npz` preserves the final attempted state when memory
allows. Both record optimizer moments, steps, batch RNG, corpus and code hashes,
runtime, and the validation loss at that state. An unscored final state has a null
loss. `--initialize-from` continues in a fresh directory with the parent's
weights, AdamW moments, steps and batch RNG. It requires matching model, corpus,
runtime, batch/optimizer settings, source components, and reproduced validation.
Same-code CPU split runs reproduce uninterrupted updates exactly. Only the
explicitly pinned original GPU trainer revision is accepted across the addition
of this continuation feature, with all numerical and batching components
unchanged; that transition is recorded as a reviewed code-revision continuation.
Reports distinguish local updates from global steps and cumulative targets.
Checkpoint random-start provenance denotes our model family's origin; a continued
run's metadata identifies its actual trained parent. Unknown files and
checkpoints from another run cannot be overwritten.

The local M4 Max benchmark measured three complete optimizer updates after one
warmup: 0.256 seconds per update on average, with about 5.54 GB of GPU driver
allocations. Actual wall time, supervised tokens, selected step and memory are
saved in each run report. The benchmark does not predict final language quality.

The first 5,395,201-parameter GPU pilot completed 1,000 updates in 265.8 seconds,
processing 3,556,921 supervised byte/EOD targets. Fixed validation loss fell
from 5.7539 to 2.0201 and selected update 1,000. The writing exam failed:
none of the 40 recorded greedy or sampled continuations was understandable,
complete language that followed its premise. All reached the generation budget.
Declining validation loss supports testing more training; it does not establish
that additional updates will produce coherent language. Preserve this failed
exam. Reusing its results to plan further experiments makes it exploratory;
final qualification needs fresh, frozen probes.

`continuation.json` predeclares three additional chunks of at most 1,000 updates
each, with a 500-second per-chunk wall cap and 1,800-second chain cap. The fresh
`language_exam_v2.json` was frozen before those chunks. Its full prompts are absent
from the prepared corpus; similar phrases and near-duplicates are not excluded.
Only validation selects checkpoints during the chain; generate fresh writing
probes after that stage rather than at intermediate steps.

`language_exam.json` freezes 20 separately authored continuation prompts and
explicit teacher-review criteria. Test stories and exam prompts must stay out of
training and checkpoint selection. Lower validation loss alone cannot pass the
language milestone. Store raw generations, including failures, and assess
readability and continuity after selecting the checkpoint using validation.
The provisional thresholds are pilot decisions, not a validated benchmark.

The current Hive system text with tools disabled and empty lessons already uses
1,132 UTF-8 bytes, before user input, lessons, history or role framing. The
2,048-byte language pilot therefore does not establish enough context for normal
Hive conversations. Later work needs a measured dialogue/context design, learned
turn endings, compatible serving, and independent B3/tool evaluations.
