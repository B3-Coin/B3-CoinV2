"""Check the explicit synthetic header-count expectation in retained output.

This checks recorded observations, not source execution, BFT safety or liveness.
Baseline failure is intentional; no old probe or old protocol API is changed.
"""
import argparse
import json
from pathlib import Path


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("results", type=Path)
parser.add_argument("--max-headers", type=int, required=True)
args = parser.parse_args()
if args.max_headers < 1:
    parser.error("--max-headers must be positive")

rows = []
try:
    for line in args.results.read_text().splitlines():
        value = json.loads(line)
        if type(value) is not dict:
            raise ValueError("result record must be an object")
        if value.get("type") in ("source_provenance", "completed"):
            continue
        if type(value.get("inputs")) is not int or type(value.get("headers")) is not int:
            raise ValueError("measurement must contain integer inputs and headers")
        if value["headers"] < 0:
            raise ValueError("header count cannot be negative")
        rows.append(value)
    if [r["inputs"] for r in rows] != [100, 200, 300]:
        raise ValueError("require exactly the 100/200/300 observations, in order")
except (OSError, ValueError) as error:
    parser.error(str(error))

failed = False
for row in rows:
    passed = row["headers"] <= args.max_headers
    failed |= not passed
    print(json.dumps({"check": "retained_header_count_bound", "inputs": row["inputs"],
                      "headers": row["headers"], "maximum": args.max_headers,
                      "result": "PASS" if passed else "FAIL"}, sort_keys=True))
raise SystemExit(1 if failed else 0)
