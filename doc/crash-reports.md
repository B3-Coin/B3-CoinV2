# Crash reports

When `b3coind` or the GUI dies from a fatal error, it writes a short text file
to the `crash_reports` directory inside the network data directory, for
example `%APPDATA%\B3-CoinV2\crash_reports\crash-20261013T114221Z-36916.txt`
on Windows mainnet. The next start logs the path, and the GUI shows it once
with a button that opens the folder. `-crashreports=0` turns the reporter off.

Covered: segmentation faults and other fatal signals, Windows exceptions
(access violation, stack overflow, heap corruption), `abort()`, failed
`Assert` checks, plain `assert()` (on Windows the GUI keeps stderr in
`crash_reports/stderr.log` and the report quotes its tail) and uncaught C++
exceptions.

## What a report contains

- version, operating system and start time
- the reason (signal or exception code, fault address)
- the name of the crashing thread (`init`, `loadblk`, `msghand`, `qt-init`, ...)
- the assertion or exception message, if any
- the call stack, each frame as module base name plus offset

It contains no memory contents, keys, wallet data, addresses, balances or log
lines, and nothing is sent anywhere: users attach the file to a bug report
themselves. Only module base names are written, so the user's home path does
not appear. An assertion or exception message can contain a source file path
of the build machine.

## Reading a report

Offsets are relative to the module's load address. Symbolize them with the
debug symbols of exactly the build named on the `version:` line, so keep the
unstripped binaries (or separate debug files) of every published build.

Linux and macOS (`b3coind+0x135f008`):

```sh
addr2line -f -C -p -e b3coind 0x135f008
```

Windows (`b3coin-qt.exe+0xc10080`): add the image base, which is
`0x140000000` for the MinGW builds (`x86_64-w64-mingw32-objdump -p` shows it):

```sh
x86_64-w64-mingw32-addr2line -f -C -p -e b3coin-qt.exe 0x140c10080
```

Frames in system libraries (`ntdll.dll`, `KERNEL32.DLL`, `libc.so.6`) usually
only show where the crash was dispatched; read the first frames that name a
B3 module.

## Limits

- On Linux and macOS a stack overflow is reported only on the thread that
  installed the reporter (the alternate signal stack is per thread). Windows
  reports stack overflows on every thread.
- The reporter is installed once logging has started, so a crash before that
  (argument parsing, data directory selection) leaves no report.
- `-crashreporttest=<kind>` (debug option) crashes deliberately right after
  installation; the functional test `feature_crash_report.py` uses it.
