# Bounded retry of transient result-evidence refusal

Profile: `outcome-upgrade-transient-budget-v1`. This is a local read policy,
not consensus, an agreement timeout, an economic retry or a new API format.
The source has a separate read-only review PASS. Its six-case native evidence
suite and complete 28-suite native selection passed locally.
The [failed captured burst](b3-flowmesh-post-admission-diagnostics-20261016.md)
remains unresolved: this deterministic counterexample is not its proven cause.

## Reproduced problem

An included original can still lack proof of what it executed. The client may
ask the endpoint that supplied its inclusion for the matching result preimage.
Previously it permanently marked that endpoint attempted before the read.
A temporary standalone BUDGET refusal therefore prevented in-process recovery
even after genuine evidence became available. Inclusion remained verified and
the original was never resent; the missing result did not mean a failed trade.

The real-TLS test-first control returned exit 201, 767 passing assertions and
three failures after all actors drained: the later result stayed unverified,
its repeated observation stayed unverified, and the evidence-read count stayed
one instead of two. Exact action bytes, inclusion, durable no-resubmit state
and all no-send assertions passed.

The original prospective fixture SHA256 was `950f1817` (prefix). Root found
before execution that Stop resets the server port to zero; the retention-only
restart now reuses the generated endpoint captured before Stop. That initial
compiled fixture was retained but not executed. The corrected BEFORE test
source SHA256 is
`5f89f26db221f11b3838201801b67f0e42cfc508a7efdcb769830d8c6d01e869`;
binary SHA256 is
`27d0d0e924a588b2c50c338860ad1c49c154a19713572d5c8bfa1b425b187684`.
The original permanent-EXPIRED test and every earlier case body are unchanged.

## Minimal correction and bounds

Each pending original has eight fixed volatile attempt records, matching the
maximum eight index-stable endpoints. Each record has a terminal flag and a
monotonic retry time. At most 512 pending originals give at most 4,096 such
records; padding and unrelated buffers are separate from that logical bound.
There is no allocating retry map, unbounded history or persisted schema change.

The terminal flag is still set before transport. Only a parsed `ok:false`
response containing the exact existing standalone public BUDGET error reopens
this read, with a fixed one-second cooldown starting after response parsing.
Refused polls neither move that time nor charge a token. Afterward a request
still needs the unchanged global automatic-read budget, current authority,
retained proof, advertised capability and endpoint cooldown checks.

There is one request to the same endpoint per attempt, no failover loop, and
the existing five-second request/reply limits remain. Extra evidence-token
charging is unchanged. Invalid/malformed evidence, EXPIRED, TOO_LARGE,
unsupported responses and transport failures remain terminal one-shot attempts
in this stage. An embedded action-result BUDGET alone does not reopen a
standalone attempt. Repeated genuine BUDGET responses can retry indefinitely
in elapsed time, but remain rate- and storage-bounded. No eventual evidence
availability is promised for a permanently unhelpful endpoint.

Current-authority verification, inclusion labels, exact signed bytes,
no-resubmit markers and the existing synchronous verified-return save remain
unchanged. If reason-code allocation fails after an already recognized BUDGET,
the installed cooldown still binds the reopened read; it does not grant
authority or remove any obligation.

## Diagnostics and test limits

Numeric diagnostic fields use only the existing outer ActionStatus span,
which outlives its work lock. No inner emitting span or logging default is
added. These fields are observations, not authority; the existing BENCH-off
capture requirement and drop checks still apply. Early-return waited paths
are outside this ordinary-status diagnostic coverage.

`outcome_upgrade_decision` values are:

| Value | Observed branch |
| --- | --- |
| 1 / 2 / 3 / 4 | No verified inclusion / outcome already verified / no retained proof / unknown endpoint |
| 5 / 6 / 7 / 8 / 9 | Not advertised / terminal attempt / endpoint backoff / evidence cooldown / global budget |
| 10 / 11 / 12 | Request started / no HTTP response / malformed envelope |
| 13 / 14 / 15 / 16 | Terminal refusal / recognized transient BUDGET / verified evidence / unusable evidence |
| 17 / 18 | Caught local exception / retained entry hash mismatch |

Value 17 does not imply that an already recognized BUDGET was made terminal.
No acquisition or unlock time is inferred from these branch values.

The focused control leaves the unchanged scheduler window, gets one actual
standalone BUDGET, then exposes the genuine prebuilt proof. At response plus
200 ms it checks that the local cooldown still suppresses a second read even
though global burst tokens have refilled. At plus 1,100 ms it requires exactly
one more evidence read, still one inclusion read, one catalogue read and zero
submissions. A repeated result must be sticky. Restart inspects retention only
and cannot manufacture in-process recovery.

The seeded cancellation is honestly `included_not_executed` with
`INCLUDED_BAD_SEQUENCE`, not an applied-fill or latency measurement. This
control does not test transport-failure recovery, machine power loss, live
recovery, the five previously unexplained burst failures or 200 ms performance.

## Actual successor qualification

The frozen client SHA256 is
`b026337eb308dd90906b7e7891028b0ba4b37b52b33bdc5b7ad6e70d9cdf3bc3`.
The test translation unit is exactly the corrected BEFORE `5f89f26d` above;
the production change, not a relaxed test, changes its result. The ordinary
AppleClang 17 Release native binary SHA256 is
`24424b0deb86aab7a7d7a8afb857ff95f052c35e1e0e8f8e0973cb3d6e72f718`.
The actual six-case `flowmesh_client_outcome_evidence_tests` child exited 0:
**six passed, 4,439 assertions passed, zero failures and no selected skips**.
This includes the unchanged permanent-EXPIRED control. Unselected Boost cases
are not represented as executed tests. The separate reviewer inspected source
and fixture interactions but did not execute this native campaign.

The same build includes the separately reviewed Qt Catalog candidate. Its
complete offline Qt workspace child exited 0 with 362 passes, no failures and
three existing opt-in visual skips. That is integration evidence, not an
execution-result or latency proof. The complete version 7 native host selection
then passed **489 cases in 28 suites**, every actual child exit 0, no selected
skips, in 224.590 seconds against that same preserved native executable.
Linux/TSan, remote CI, power loss, live
recovery and the actual failed burst cause remain unqualified.
