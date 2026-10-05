# B3 student AI experiment

Branch: `b3-studenr-ai`. This is an experimental assistant trained from our own
randomly initialized model. It is not qualified for general assistant testers.

The current byte/EOD Transformer has 5,395,201 parameters, six layers, width 256,
eight attention heads and a 2,048-byte context. The latest counterfactual stage
keeps this architecture and continues our earlier mixed language and B3 model;
it does not import pretrained model weights.

## Latest training and fresh test

The latest training stage completed 1,000 updates and selected update 800 using
validation. The original 64-pair grounding gate passed only 3 pairs. The saved
selected state was then tested against its preserved parent on the same 80 fresh
cases per model. No training, response repair, sample retry or checkpoint
selection happened during that fresh test. Tools returned synthetic fixtures;
no live node, wallet or market service was contacted.

| Fresh test | Current model |
| --- | ---: |
| Requested value among several fields | 5/48 |
| Both answers correct when the requested value changes | 1/24 pairs |
| Copying a number when only one count is present | 6/8 |
| Missing requested field handled as unknown | 0/4 |
| Failed diagnostic handled as unknown | 3/4 |
| Correct tool choice | 1/4 |
| Complete accurate tool task | 0/4 |
| Useful chat | 0/4 |
| Correct B3 explanation | 0/4 |

For example, the current model copied `188` correctly but returned `8` for `78`.
It chose the correct market tool, then returned `0000` for a two-market result.
The numerical audit found 13/48 replies copied the wrong supplied count.

These are small diagnostic sets. Each wording/range/field cell has one pair.
The parent had not learned the new answer-mode prefix, so strict before/after
format scores do not isolate improved understanding. One parent reply correctly
reported ten connections in a longer sentence while failing the requested short
format. The independent reviews are AI reviews, not human qualification.

[Saved evidence](counterfactual-2026-10-05/) contains the frozen exam, exact raw
generations from both models, mechanical scores, independent reviews, original
diagnostic driver and training report. The evidence files are byte-for-byte
copies of the local records. Their hashes and checkpoint identities are in
[manifest.json](counterfactual-2026-10-05/manifest.json).

## Where the learned state is saved

| State | Local repository path |
| --- | --- |
| Current selected checkpoint, update 800 | `build-ai/counterfactual/run/model.npz` |
| Final audit state, update 1,000 | `build-ai/counterfactual/run/last.npz` |
| Preserved parent | `build-ai/tool-learning/mps-stage-2/model.npz` |

The selected checkpoint is about 62 MiB and contains learned parameters and
resumable training state. These files are git-ignored and **are not included in
this source publication**. Cloning this branch does not restore the trained AI.
The manifest records their sizes and SHA-256 identities; a separate checkpoint
backup is required to preserve the learned state beyond this Mac.

Training material lives in `contrib/b3ai/teaching/b3_curriculum.json`,
`contrib/b3ai/tool_learning/curriculum.json` and the new
`contrib/b3ai/counterfactual/curriculum.json`. These are teaching inputs, separate
from model parameters. Hive chat history remains in session memory. Its custom
reference lesson store uses the Qt application-data directory plus
`student-ai/lessons.json`; saving a reference lesson does not train the weights.
No existing custom lesson file was found during the local audit.

The latest byte/EOD checkpoint was tested through local inference. The earlier
sanity-model server adapter does not serve this checkpoint, and this training
stage does not establish working Hive chat integration.

## Validation and reproduction prerequisites

Publication checks passed all 24 selected data, scoring, plan and seal tests:

```sh
PYTHONPATH=.:contrib/b3ai/counterfactual/tests python -m unittest \
  test_data.FrozenData test_evaluate.Scoring \
  test_train.PlanRules test_freeze.SealedSets -v
```

These checks do not train a model. The toy trainer and recorder tests were not
run for this publication check. The existing Torch/NumPy environment was used.

The frozen source and prior-probe snapshot can be inspected in a clean clone.
Reproducing the original training also requires the hash-pinned local parent
checkpoint, prepared TinyStories corpus, Torch/NumPy runtime and earlier
diagnostic exams listed in `contrib/b3ai/counterfactual/plan.json` and
`prior_probes.json`. Those local build inputs are not part of the source push.
Tests requiring them explicitly skip when they are absent. Fresh seal creation
and regeneration of the prior snapshot require the earlier exams as well.

The historical test driver expects a folder three levels below the repository
root and writes exclusively to that folder. To reproduce its mechanical score
without running inference, copy `diagnostic.py`, `exam.json`, `previous-raw.json`
and `current-raw.json` from the evidence directory into a new folder such as
`build-ai/counterfactual/score-replay`, then run:

```sh
PYTHONPATH=. python build-ai/counterfactual/score-replay/diagnostic.py score
```

Use a fresh output folder; existing score files are never overwritten. This
score replay checks numerical results only. The broader chat, B3 and complete
tool-task findings come from the separately saved semantic review.

The historical scorer can label a substituted synthetic generator as meeting
its gate in test mode. Such a result is a scorer test, not evidence of student
ability. The real student run used the pinned neural generator and failed its
gate. No historical training source was changed during publication.
