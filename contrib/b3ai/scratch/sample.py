# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Sample bytes from an experimental scratch checkpoint, without node access."""

import argparse
import json
from pathlib import Path

from .checkpoint import load_checkpoint
from .tokenizer import ByteTokenizer


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("checkpoint", type=Path)
    parser.add_argument("--prompt", default="B3 is ")
    parser.add_argument("--tokens", type=int, default=128)
    parser.add_argument("--temperature", type=float, default=0.8)
    parser.add_argument("--seed", type=int, default=42)
    args = parser.parse_args()
    try:
        model, _, metadata = load_checkpoint(args.checkpoint)
        tokenizer = ByteTokenizer()
        ids = tokenizer.encode(args.prompt)
        if not ids or len(ids) > model.config.context_length:
            raise ValueError("Enter a nonempty prompt within the checkpoint's byte context limit.")
        sampled = model.generate(ids, args.tokens, args.temperature, args.seed)
    except (OSError, ValueError, TypeError, KeyError, FloatingPointError) as error:
        parser.error(str(error))
    print(json.dumps({"readiness": metadata.get("readiness", "experimental"),
                      "stage": metadata.get("stage", "unknown"), "parameters": model.parameter_count}))
    print("Experimental text continuation; not a verified assistant answer:")
    print(tokenizer.decode(sampled))


if __name__ == "__main__":
    main()
