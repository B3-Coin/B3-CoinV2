# Private closed-test report

- Candidate/version and executable/archive SHA-256:
- OS version, architecture and hardware:
- Timestamp and timezone:
- Exact page and operation:
- Reproduction steps:
- Expected / actual result:
- Selected public ActionId/sequence and market AssetIds, if relevant:
- Screenshot/short recording/OS crash report (only with consent):
- For a crash: executable UUID and matching diagnostic-symbol identity:
- Did a normal Quit complete? Exact exit/log evidence, if available:

Do not send passwords, seeds, WIF/private/BLS/operator keys, wallet files,
signer journals, RPC cookies, TLS private keys or unreviewed logs. Redact public
addresses/usernames when they identify a holder unnecessarily. Keep a private
original before redaction. No automatic upload/telemetry is requested.

For a sync/fork report also record, read-only, at two timestamps:
network/build, blocks, headers, best-block hash, peer block-download state,
chain-tip/finality status and last successful tip advance. Disabled finality
signing is not itself proof of a block-download stall. Owner-identified wrong-
fork cases are labelled individually; do not assume every percentage stall has
the same cause. Do not invalidate blocks, replace anchors, reindex blindly or
delete chainstate/lockfiles/signing history.

The owner must designate a private group reporting destination before external
handoff. A public GitHub PR is for sanitized source review, not holder data.
