# Result FIFO: one-shot before/after observation

Date: 2026-10-16. Implementation:
`b42705b021cc9286b9eb230cc16c70c1528e4eec`.
Measurement checkout: `6c8af3766afb65638a42737aa409b69fec1ad209`.

This is one predetermined comparison after the [FIFO repair](b3-flowmesh-outcome-fifo-20261016.md).
It preserves the failed control. It does not establish sustained reliability,
causal attribution for every old timeout, a latency improvement or 200 ms
qualification. No production or existing tester network was used.

## Fixed profile and exact identities

Both sequential arms used the unchanged
[performance driver](../../test/functional/feature_flowmesh_performance.py),
SHA256 `f344f81341aa4d4fd568ea491ff7e7f0695ff84b25440fe4baffda5e77412121`:
four generated validators, two TLS endpoints, one engine-off client daemon,
four buyer accounts across two markets, a funded maker, one isolated fill,
one idle second and eight burst originals. Seed was 24092026; port seed 92751.
Each arm had a separate fresh generated regtest/cache directory. Original
five-millisecond polls, 60-second action deadlines, zero SubmitWait and economic
instructions were unchanged. No public-body, RPC or native-memory capture was
enabled; BENCH/debug remained off through measured work. No build or heavy test
ran during either arm. Hardware remained the Apple M4 Max/128 GiB Mac used by
the overnight functional qualification.

Preserved daemon SHA256:

- BEFORE, native client `bff8dac1`:
  `06f5ddfdf80ffd8e472efd69aacee52978a605a69082575d7f05dedfaf71a95c`.
- AFTER, native client `f6610edb`:
  `f9fd3bb847d02f2d308f014019cb8298d3b15f711cf84badd05270a7d1f80770`.

The private collector used an explicit binary override, a 420-second process-group
deadline per arm and bounded output. It launched AFTER only after BEFORE's
inherited shutdown records showed all five actual daemon exits were zero.
There was no reselection, replacement economic action or retry-until-green loop.
Separate source review of the collector passed; the reviewer did not execute it.

## Actual results

| Observation | BEFORE | AFTER |
| --- | --- | --- |
| Parent test exit | **1** | **0** |
| Isolated verified execution result | 205.298 ms; one sample | 294.613 ms; one sample |
| Burst certified inclusions | 8/8 | 8/8 |
| Burst verified execution results | **4/8; four deadline failures** | **8/8; zero failures** |
| Burst conventional median | Not reported: incomplete population | **1,498.190 ms** |
| Burst nearest-rank empirical p50 | Not used as a successful baseline | 1,287.603 ms |
| Burst maximum verified result | Not used as a successful baseline | 2,491.237 ms |
| Follow-up read consistency | No recorded read failures; workload still failed | Passed |
| Maker conservation check | Not executed after incomplete workload; report flag false | Passed |
| Harness threads stopped | Yes | Yes |
| Final daemon exits, indexes 0–4 | All actual 0 | All actual 0 |
| Performance flag | False | False |

AFTER's eight result times, in sorted order, were 726.215, 905.968, 1,236.931,
1,287.603, 1,708.776, 1,913.307, 2,037.073 and 2,491.237 ms. Its conventional
median is the mean of the middle two, not the nearest-rank p50.

The collection wrapper exited **1**, correctly retaining BEFORE's failure even
though AFTER passed. BEFORE's failed originals all had verified inclusion but
did not obtain verified execution outcomes within their original deadline.
The run did not capture enough internal decisions to assign each timeout to a
particular budget, endpoint or FIFO event. No funds-loss conclusion follows from
the unexecuted maker check, and no later instruction was sent to replace a
failed original.

The isolated AFTER sample was slower than BEFORE. Successful AFTER completion
and the integrated failing-before regressions support the narrow scheduling
repair; they do not prove improved median latency or explain all older failures.
The 200 ms durable client-verified goal remains unmet. This harness itself still
declares power-loss/durable-client-outcome qualification false; it is not Qt,
WAN, multiple-client-process or sustained-load qualification.

## Evidence preservation

Private collector SHA256:
`2d7b5c035bfb1ae1ba31fe8f5deded335580fbce95ea3fb835feb86409114cb2`.
Final actual-exit manifest:
`a210bf93185099cc95a6272909a95289983b0c5d69fdd2e52bcd52123387a4e5`.
BEFORE full report:
`83248ca7729f92b6259c9905eb3daef26e6d93e4bd1a98af67f4067be1aeaf8d`.
AFTER full report:
`0ed38e6bfd076bbce94d5f69d668ac137b88f2e9d74da0152e039ac0215a08fb`.

Generated wallets, private TLS material, journals, databases, raw stdout/stderr
and full test directories remain private. Earlier failed captures and the
[separate older decision slice](b3-flowmesh-outcome-slice-20261016.md) remain
unchanged. Their timings are not combined with this pair.
