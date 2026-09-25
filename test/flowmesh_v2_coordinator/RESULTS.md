# First coordinator-selection result

Development comparison completed successfully on 2026-09-25.
Base: `82ebbb2e31ae8df09b575d379117c55490461617`.
Predeclared plan: `0e070cf195d63bae15d23d79c13e2a8e064ee47e`.
Tested implementation: `233e4f6df41338d3772d12361bfa4909a6cfd76b`.
The complete 352-test committed-tree result is in
[QUALIFICATION.md](QUALIFICATION.md).

**Finding: changing proposer tenure alone does not shorten healthy agreement
in this model.** The complete coordinator/pipeline proposal has not been
implemented by this experiment.

## Comparison

Units below are logical simulation ticks, **not milliseconds**.
First verified durable result and application on every required replica were
at the same tick in each completed case; they remain separately captured.
Every successful case included real accounting-model matching, not only pool
acceptance.

| Case | Per-batch rotation | Four-batch tenure |
| --- | --- | --- |
| Healthy, eight consecutive batches | 3 ticks each | 3 ticks each |
| Eight instructions, one four-fill batch | 3 ticks | 3 ticks |
| Seat 0 absent, four batches | 35, 3, 3, 3 | 35, 35, 35, 35 |
| Seat 3 absent, four batches | 3, 3, 3, 35 | 3, 3, 3, 3 |
| Two of four seats absent | No commitment in bounded run | No commitment in bounded run |

Healthy batches issue one PROPOSE, four PREPARE and four COMMIT objects under
both policies. Neither policy needs an extra election phase at a normal
sequence boundary in this model; rotating a deterministically known proposer
was therefore not an extra healthy-path network round to remove.

The seat-0 failure is a preserved negative result. Every new sequence resets
to view zero under the existing rules. A four-batch tenure therefore retries
the absent seat at each sequence, rather than remembering the successful
replacement. The existing timeout/view-change path recovers safely but repeats
its wait. No timeout was shortened and no peer was silently removed.

The seat-3 case is a short-window advantage, not permanent fault tolerance:
seat 3 becomes the scheduled leader at sequence 3 in the control, but only
later in the tenure schedule. A longer run would eventually reach that seat.
Both approaches retain the same quorum requirement and fault assumption.

## Accounting and hosting interpretation

Final accounting digests match between both policies for each workload.
For one completed two-action trade, the buyer receives one base atom and
spends 20,001 quote atoms; the seller gives one base atom and receives 19,999
quote atoms. The remaining two quote atoms follow the unchanged model fee
distribution. No hosting deduction is introduced.

With four equal seats on independent hosts, one unavailable host leaves the
three required votes. If one host runs two of the seats, losing that host
leaves only two: both policies stop. Putting all four seats on one host makes
all four unavailable together. These tests model availability; they do not
establish independence, key safety or censorship resistance of actual hosts.

## Retained comparison evidence

The full synthetic capture is retained outside the source tree as
`comparison-development.json`, 4,007,878 bytes:

`SHA256 2ee99288eb791ee8971fb0a5c2779ebed4828f7637ad1c1ea037df482f24918b`.

[Compact results](RESULTS.json) retain every comparison's durations,
message counts and final accounting digest. Use [the documented command](README.md)
to regenerate full event/signature evidence; the measurement boundary and
synthetic assumptions must accompany any comparison. No existing native
performance capture was overwritten or relabelled.

## Next gate

Do not promote this four-batch rule as the solution to the 200 ms target.
The smallest next design task is specifying persistent, agreed replacement
authority across batches and its restart rules. Private last-seen leaders or
first-received certificate subsets cannot determine that authority.

The native speed hypothesis still requires its own experiment: remove
demonstrated queue waits/repeated work and measure one complete durable
agreement path through the client. Replacing native V1 certification also
requires a defined durable proof/history boundary; merely deleting that step
would not preserve its guarantees. No such replacement was made here.
