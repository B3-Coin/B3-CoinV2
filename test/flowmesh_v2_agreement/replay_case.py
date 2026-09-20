"""Replay one minimized regression against an explicit source tree.

Emits synthetic evidence only: observed events, all issued messages and the
unittest outcome. It does not run nodes, sockets or wallets. Output can be
large but is bounded by the selected test profile. Failures remain failures.
"""
import argparse
import io
import json
from pathlib import Path
import sys
import unittest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--case", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    source = args.source.resolve()
    sys.path.insert(0, str(source / "test/flowmesh_v2_agreement"))
    from fm_simulator import Simulator
    from fm_protocol import PROFILE
    observed = []
    original = Simulator.__init__

    def capture(self, *positional, **keywords):
        original(self, *positional, **keywords)
        observed.append(self)

    Simulator.__init__ = capture
    stream = io.StringIO()
    result = unittest.TextTestRunner(stream=stream, verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromName(args.case))
    # No developer-local paths in retained synthetic evidence.
    outcome = stream.getvalue().replace(str(source), "[SOURCE]")
    evidence = {"schema": "flowmesh-agreement-regression/1", "revision": args.revision,
                "case": args.case, "profile": PROFILE["profile_id"],
                "success": result.wasSuccessful(), "test_output": outcome,
                "simulations": [json.loads(sim.retain_trace()) for sim in observed]}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"case": args.case, "success": result.wasSuccessful(),
                      "simulations": len(observed),
                      "events": sum(len(s.trace) for s in observed)}))
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
