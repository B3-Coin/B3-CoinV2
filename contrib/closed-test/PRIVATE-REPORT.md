# Private closed-test report

Send only to the coordinator's explicitly assigned private destination.
There is no automatic upload or telemetry. If no destination was assigned,
keep the report locally and ask for it; do not post private logs publicly.

- Candidate ID, source commit/export identity, executable/archive SHA256:
- macOS version/build, hardware/architecture, Qt version from manifest:
- Timestamp and timezone:
- Exact page, selected wallet/test account, market/AssetId and operation:
- Numbered reproduction steps (ordinary mouse/keyboard):
- Expected result:
- Actual result; selected ActionId/sequence before and after if relevant:
- Native exit status and new Shutdown done, or crash/hang details:
- Consented screenshot/short recording or matching macOS crash report:
- Whether reproducible; which independent attempts:

For a sync/fork report add two timestamped read-only observations of build,
blocks, headers, best-block hash, peer inflight/download state, chain tips,
finality status and a bounded complete log interval around last UpdateTip.
Keep each installation separate. Do not invalidate/reconsider/precious blocks,
replace anchors, delete chainstate/lockfiles or reset signing history.

Before sharing, review logs/screenshots for identifying addresses, paths and
credentials. Never send seeds, WIF/private/BLS/operator keys, passphrases,
wallet files/backups, RPC cookies/auth tokens or signing journal contents.
For the unresolved accessibility crash, include the original macOS .ips
report only after privacy review, plus exact candidate hash and preceding
normal UI steps. Do not run intrusive diagnostics without separate consent.
