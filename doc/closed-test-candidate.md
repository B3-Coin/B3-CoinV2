# Closed testing: candidate 04

This is an isolated test candidate, **not a public production release**.
Use generated wallets and valueless regtest coins only. Never open a holder's
wallet in this candidate or import a real spending, FN or BLS key.

## Scope and boundaries

Ordinary traders use Qt, local signing and verified remote HTTPS with
`-enableflowmeshvalidator=0` (also the ordinary default). No FN Coin is required
to trade. Operators opt in explicitly; authorized seats still require arming
and the existing durable signing/anchored eligibility checks.

The market is canonically colored-asset/B3. Its reverse B3 presentation uses
conservative reciprocal arithmetic and **token quantity caps**, not a guarantee
of an exact B3 quantity. Review the exact AssetId, canonical instruction and
reservation before confirming. Trading fees remain **B3**; stable-asset fees,
new markets and settlement changes are not included. Test dollar labels do not
indicate backing or redeemability.

FMN2 operator networking is authenticated **plaintext TCP**, confined to the
controlled loopback harness. Client HTTPS does not encrypt operator traffic.
No QUIC, WAN, power-loss or public-service qualification is implied.

## Current board

| Area | Latest demonstrated scope | Remaining limitation |
| --- | --- | --- |
| Delivery and client | Bounded independent transport; ordinary remote client; exact signed-action retention and no-resubmit protection | Integrated rerun is recorded in the candidate manifest; earlier runs remain earlier evidence |
| Settlement compatibility | Generated enabled/disabled nodes exercised actual valid and invalid checkpoint/withdrawal blocks | Not a production deployment or ordinary propagation guarantee |
| HTTPS | Oversized rejection before dispatch and bounded cleanup repaired; later focused and combined passes supersede the earlier intermittent failure | A current-tip reconciliation refusal is distinct; historical HTTP400s without bodies remain unattributed |
| Qt | Demonstrated lifetime/callback, receipt/status and redundant-refresh repairs | Original macOS accessibilitySelectedChildren SIGSEGV OPEN / UNRESOLVED; no recurrence is not a fix |
| Saved requests | Exact instruction/reopen and automated attribution coverage | Ordinary A-to-B selector/cross-selection screen coverage must be recorded explicitly, not inferred |
| Migration | Generated strict-reader, staged replacement, handled failure and process-interruption checks | Not universal legacy recovery; encrypted historical variants/BDB log replay, Windows directory durability and power loss remain unqualified |
| STAKE | Native generated encrypted-owner selection, review, Cancel, separate owner spend and reopen retained | Watch-wallet screen switch is a separate targeted check; no inherited spending authority permitted |
| Reconciliation | Read-only current-UTXO comparison with explicit incomplete results | Not complete history, derivation coverage or automatic balance repair |
| Community sync | Owner attributed identified investigated installations to wrong forks | This attribution does not close other installations without matching evidence; wallet patches do not change fork choice |

The release manifest separates fresh integrated passes from retained evidence.
Any newly reproduced authority, funds-safety, instruction-loss or consensus
defect blocks the affected test capability; it is not just a known UI issue.

## Local self-contained mode

See `contrib/closed-test/` for the fresh loopback environment and package helpers.
A local test operator builds the pinned source, starts four disposable operators,
and obtains a session-specific public HTTPS profile. The client is built using
that profile through the guarded build option, then packaged and extracted.
Generated TLS private keys, operator wallets, cookies, client wallets and outboxes
stay outside the source/archive. They are never distributed or shared.

The environment operator keeps the harness running during the session and stops
its owned children normally afterward, preserving data. Expired profiles or
stopped endpoints mean the session is unavailable, not that a wallet should be
reset. A new local environment uses new keys and a successor profile/package;
do not edit a frozen profile to evade its guard.

Loopback URLs are not external tester endpoints. Ordinary production traders
are not expected to run this four-operator test harness.

## Ordinary tester checklist

1. Check the candidate identity, archive checksum, supported Mac architecture and
   actual minimum OS in its manifest. Follow only the approved signing route;
   do not disable Gatekeeper or other system protections.
2. Start the explicitly approved loopback environment, or obtain an independently
   approved private shared environment. Verify it is actually reachable.
3. Open the extracted guarded client and create your **own new test wallet**.
   Confirm regtest, correct wallet identity and validator engine OFF.
4. Give the harness only a newly generated public receiving address for valueless
   test funding. Do not transmit keys or wallet files.
5. Open the B3-facing market and inspect its exact configured AssetIds/decimals.
6. Deposit only generated test funds; distinguish B3 confirmation, admission and
   certified FlowMesh credit. Do not duplicate a deposit while waiting.
7. Review one new test order: market/account, Buy or Sell direction, canonical
   instruction, token cap, reciprocal price, fee and reservation. Submit once.
8. Observe certification separately from admission and separately from a fill.
   Review and submit a distinct cancellation once; closing the UI is not a
   cancellation.
9. Open Saved requests; click its drop-down and select the order, then the
   cancellation using ordinary mouse/keyboard. Verify each card's ActionId and
   sequence changed to the selected request. If this does not work, capture it
   and report it; do not repeatedly use inaccessible automation or create trades.
10. Request status during refresh. Confirm the queued indication and correct
    selected-card attribution after selection changes.
11. Quit normally, retain the actual clean exit, then reopen the **same wallet**.
    Check original ActionIds, sequence/high-water and saved requests. An unknown
    outcome can retry only identical signed bytes under existing rules. A
    certified instruction must not become a new submission.
12. Quit, stop the session-owned environment, and preserve your generated data.

## Optional advanced wallet checks — separate from trading

Migration: generated fixtures or separately approved disposable copies only.
Cleanly stop the legacy application; preserve the complete wallet and relevant
BDB environment/logs; verify a consistent backup; use the documented supported
path in `b3-wallet-migration.md`; keep originals afterward. A main-file copy
does not establish unflushed-log completeness. The tested flat-file rename-gap
recovery is explicit exclusive offline recovery, **not automatic**. Never
advertise process-interruption tests as power-cut guarantees or community-wallet
recovery. No permissive decode or historical nTime/txid rewrite is authorized.

STAKE: ordinary selection excludes STAKE. Explicit inclusion displays warnings,
ownership, maturity, lock and pending/active status. Review the exact consumed
outpoint, recipient, fee and ordinary change. Cancel means no record/broadcast;
the existing review can already have prepared/signed a transaction and reserved
a change key. Do not claim no transaction was constructed. For the remaining
watch-only check, switch from generated owner to generated watch wallet and
confirm owner authorization/send readiness does not carry over, then return to
the owner and recheck identity/unavailable selections. No additional spend is
needed to repeat already completed evidence.

Reconciliation: use the documented read-only tool privately. Current UTXOs are
not complete transaction history; unsupported policies/derivations may leave
coverage incomplete. A client timeout need not stop a server scan. Never abort
someone else's scan. Nonzero INCONCLUSIVE and privacy warnings are intentional;
matching totals alone do not establish recovery. No automatic repair/migration.

## Crash and private reporting

The original selected-accessible-child crash was observed on Apple Silicon
macOS with Qt 6.11.1. Keep its OPEN status distinct from repaired startup/dialog/
callback faults. Normal testers need no intrusive debugger. If it recurs, retain
the OS crash report, exact app hash, UUID and matching symbols. Record the last
ordinary selection/popup/page action; do not suppress accessibility or keep
probing through a diagnostic stop.

Use the template in `closed-test-private-report.md`. Review recordings/logs for
sensitive information before sharing. No automatic telemetry or uploads occur.
A private group report destination still requires owner designation. Until then,
provide a sanitized summary through the existing private project conversation,
not a public GitHub issue containing wallet/operator data.

## External distribution prerequisites

Not yet supplied by the local pilot: approved shared host/network and availability
operator; private B3 connection; reachable hostname-verified HTTPS endpoints;
compatible tester platform; private report destination; explicit distribution
authorization; approved Developer ID/notarization or other supported signing
route. Local ad-hoc signature verification is not external installation approval.
Windows CI results and Windows durability/installation are separate qualifications.

Future features remain out of scope: stable fees, asset/asset markets, hosting
commissions, automatic committee removal, new BFT/PoW and QUIC.
