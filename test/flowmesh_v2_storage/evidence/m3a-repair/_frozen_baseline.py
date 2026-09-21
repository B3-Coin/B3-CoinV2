"""Portable source-selection guard; not part of the model or fault schedule."""
from pathlib import Path
import subprocess
import sys


FROZEN_REVISION = "1d022dbf0da63636d6d43650d3864222761497a1"


def source_from_command_line():
    """Require one explicit, clean, detached-or-branch frozen checkout."""
    if len(sys.argv) != 2:
        raise SystemExit("Usage: python3.14 -B PROBE.py FROZEN_CHECKOUT")
    source = Path(sys.argv[1]).resolve(strict=True)

    def git(*arguments):
        try:
            completed = subprocess.run(
                ["git", "-C", str(source), *arguments],
                check=True, capture_output=True, text=True, timeout=10,
            )
        except (OSError, subprocess.SubprocessError):
            raise SystemExit("Cannot verify the provided frozen Git checkout.") from None
        return completed.stdout.strip()

    if git("rev-parse", "--show-prefix"):
        raise SystemExit("Provide the repository root, not a subdirectory.")
    if git("rev-parse", "HEAD") != FROZEN_REVISION:
        raise SystemExit("Refusing a source revision other than the frozen baseline.")
    if git("status", "--porcelain", "--untracked-files=all"):
        raise SystemExit("Refusing a modified or untracked frozen checkout.")
    return source
