# FlowMesh Windows operator identity creation

The Windows preview built at `14b63e2869a9e7c43f7c0d4f25ac2b213a443301`
imports `fopen` from `msvcrt.dll`. Its first independent-network start uses
`fopen(path, "wbx")`, whose C11 exclusive-create mode is unsupported by that
runtime. This can return the reported `Cannot create FlowMesh operator key`.
The earlier malformed-peer-pin error occurs before this operation and is a
separate configuration problem. Repeating the status RPC does not restart it.

## Narrow repair

Only the FlowMesh network identity file uses the new helper. Windows creation
uses `_wopen` with `_O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY | _O_NOINHERIT`,
then `_fdopen` on that descriptor. It never falls back to truncating `wb` on a
pathname. Windows reads use `_wfopen` so helper read/create path encoding
agrees. Other wallet I/O and POSIX creation remain unchanged. Creation/read
errors now include the operating-system errno description and number.

The existing directory lock, existing-key checks, exact 32-byte format,
`FileCommit`, close, `DirectoryCommit`, and public-identity publication order
are preserved. A partial/invalid identity is not removed or regenerated.
Windows permissions still inherit directory ACLs. Unicode helper-path checks
do not qualify the separate directory-lock path or all wallet data paths.

## Focused evidence and limits

`contrib/b3hive-release/check-flowmesh-keyfile.cpp` is a standalone generated
file fixture with no wallet, real key, RPC, signer or network. It checks first
creation, binary bytes (including NUL/LF/CTRL-Z), no overwrite, existing-byte
readback, partial-file/directory preservation and missing-parent failure.
`--reopen` checks the same bytes in a fresh process. `--legacy` runs the exact
old mode and reports its actual outcome; failure is not silently counted as a
fixed-path success.

The manual release-build input `check_operator_key=true` builds that small
probe with the same MinGW/MSVCRT toolchain and runs it on native Windows.
It requires the old creation failure and the repaired tests to pass. This
explicit check is separate from the disabled full wallet/consensus suites and
from compilation of the portable wallet package. Native Windows results and
the corresponding run/source identity must be recorded before publication.

The existing FlowMesh service round-trip/restart test is retained. A focused
unit regression also checks an unchanged public identity after restart and
refusal to replace a partial identity. Addition of a test is not execution
evidence. No affected community installation or live wallet was modified to
perform these checks.
