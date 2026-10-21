#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

"""Record public GitHub build identity in a staged desktop artifact."""

import argparse
import json
import os
from pathlib import Path
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--platform", choices=("win64", "macos-arm64", "macos-x86_64"), required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--build-only", choices=("true", "false"), required=True)
    args = parser.parse_args()

    # Never copy the full environment: it may contain credentials or paths.
    identity = {}
    for field, variable, pattern in (
        ("repository", "GITHUB_REPOSITORY", r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+"),
        ("source_commit", "GITHUB_SHA", r"[0-9a-f]{40}"),
        ("run_id", "GITHUB_RUN_ID", r"[0-9]+"),
        ("run_attempt", "GITHUB_RUN_ATTEMPT", r"[0-9]+"),
    ):
        value = os.environ.get(variable, "")
        if len(value) > 200 or not re.fullmatch(pattern, value):
            parser.error(f"Missing or invalid public build identity: {variable}")
        identity[field] = value
    if not re.fullmatch(r"[0-9A-Za-z][0-9A-Za-z.+-]{0,79}", args.version):
        parser.error("Invalid source version")

    macos = args.platform.startswith("macos-")
    build_only = args.build_only == "true"
    info = {
        "schema": 1,
        **identity,
        "source_version": args.version,
        "platform": args.platform,
        "architecture": "arm64" if args.platform == "macos-arm64" else "x86_64",
        "configuration": "Release",
        "build_only": build_only,
        "tests_executed": False if build_only else None,
        "test_policy": (
            "Compilation and packaging only; test programs and execution disabled."
            if build_only else
            "Separate workflow gates apply; consult the recorded run for their results."
        ),
        "signing": "ad-hoc" if macos else "unsigned",
        "notarized": False if macos else None,
        "minimum_os": "macOS 15.0" if macos else "Windows 10",
        "qualification": "Build identity only; not a production or mainnet qualification claim.",
    }
    with args.output.open("x", encoding="utf-8") as output:
        json.dump(info, output, indent=2, sort_keys=True)
        output.write("\n")


if __name__ == "__main__":
    main()
