# FlowMesh retained-candidate startup repair

## What changed

FlowMesh starts with its live production gate closed while B3 checkpoint and
index reconciliation runs. Previously, market installation tried to execute a
saved, uncommitted signing candidate through that same gate before startup
could open it. This could stop the entire wallet with:

```
FlowMesh runtime cannot re-execute its retained signing candidate
```

The runtime now retains the decoded and authenticated candidate in memory and
pauses that market until reconciliation permits live verification. The first
eligible tick or incoming production message re-executes the exact saved
candidate before proposing, voting, or applying a certificate. Serving already
committed history does not require signing and remains possible.

This does not delete, rewind, replace, or waive a signing lock. A candidate that
fails verification after reconciliation remains blocked. It is not a recovery
for orphaned anchors, conflicting votes, unavailable quorum, or corrupt stores.
No B3 consensus rule, committee threshold, wire format, or database format changes.

## Windows portable build

Manual `windows_build_only` and `desktop_build_only` Actions runs package a
portable Windows ZIP and SHA-256 checksum, without building the setup installer
or compiling/running tests in Actions. Normal tagged-release behavior is unchanged.
The packager checks x86-64 PE architecture and bundles any required non-system
runtime DLLs found in the build/depends toolchain; unresolved imports fail packaging.

Close the old wallet, extract the complete ZIP, and launch `b3coin-qt.exe`.
Keep any existing custom data-directory setting. The portable executable still
uses the normal wallet data directory by default: it does not create an empty
signing history beside the executable. Do not remove or rename the `flowmesh`,
`finality_signer`, or wallet directories to work around this error.

Starting successfully is not proof of FN participation or quorum. Check
`getflowmeshvalidatorinfo` and the affected market after startup. Existing signing
keys remain subject to the normal arming and committee-membership requirements.
