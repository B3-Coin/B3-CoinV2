# FlowMesh conservative catch-up tail scheduling

Status: implemented as a separate increment after qualified `8ea5251`;
executable qualification pending. This does not amend that candidate's results.

## Observed limitation

The passing four-node run had a 13.652-second maximum among 40 sequential
observations. Following verified catch-up progress, the runtime unconditionally
requested one more page. An unanswered probe at the peer's tip installed the
ordinary 15-second failure cooldown. Later authenticated ahead proposals could
not request catch-up until it expired. Recorded request intervals of 15.263 and
15.483 seconds match this source path; the initiating lost certificate's cause
was not logged. The five-second timeout, failure cooldown, and all request
bounds remain unchanged in this increment.

## Local decision, not authenticated tip information

After a matched solicited response actually advances verified state, omit the
speculative follow-up only when all three conditions hold:

1. Every returned entry verified and applied, not merely a valid prefix.
2. The response count is below the request's count limit.
3. Unused requested payload space could hold any legal additional framed
   certified entry: at least `4 + FLOWMESH_CERTIFICATE_MAX_BYTES` (2,098,180 bytes).

The four-byte per-entry length prefix is included; the two-byte page count is
already in the response payload size. The certificate encoder ceiling is a
conservative upper bound, including for the catch-up codec. Request limits are
copied before the pending request is erased, and subtraction is bounds-checked.

An honest greedy `HandleGet` sender would include another entry if it had one
under these conditions. Otherwise the existing continuation remains, including
short byte-limited pages and partially applied non-halted responses. A successful
small page clears the existing cooldown without creating a new unanswered
request, leaving later eligible ahead hints free to solicit normal catch-up.

This records no remote tip as trusted and adds no new messages, timers, request
queues, or bypass privileges. A malicious peer can supply a short valid prefix
while withholding more history, just as it can withhold all history; later
eligible hints and bounded discovery remain available. No claimed freshness,
certificate validation, permanent lock, quorum, anchor, header, or matching rule
changes. The old `v1.1.3` and `v1.1.4` source tags have the same greedy count/byte
`HandleGet` and absent empty response, supporting compatibility of this scheduling
heuristic; that source check is not mixed-version execution qualification.

## Focused coverage

- Pure predicate checks: exact maximum-entry slack minus one/equal/plus one,
  count cap, empty page, partial application, and subtraction bounds.
- Idle legacy discovery still covers new-market and reconnect recovery, now
  asserting exactly one request for each small complete page. The misleading
  test transport that silently truncated tiny 4 MiB-budget responses was removed.
- Genuine sender requests with count limit one and with a byte budget exactly
  equal to one encoded entry return unmodified bounded pages; both require
  continuation under their real limits.
- A fresh verified ahead certificate immediately recovers two missed entries
  after a small-page sync, without reconnecting or advancing the test clock.
- A valid genesis followed by an invalid trailing entry applies only genesis,
  requests the next sequence, and retains ordinary failure cooldown behavior.

This increment targets an avoidable transport delay, not permanent split-lock
recovery or a production latency guarantee.
