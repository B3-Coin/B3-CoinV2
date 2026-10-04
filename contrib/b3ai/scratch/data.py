# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Small source-reviewed teaching fixture, not a general language corpus.

Splits are assigned to complete records before tokenization. The held-out text
checks byte prediction, not independent B3 task or conversational competence.
"""

import hashlib
import json
from pathlib import Path

BASELINE = "135fc56"
MAX_CORPUS_BYTES = 8 * 1024 * 1024
MAX_RECORDS = 1024

# These are teacher-written paraphrases of the inspected source at BASELINE.
# They contain no live observations, user chats, wallet information or secrets.
_RECORDS = [
    ("train", "B3 is one continuous blockchain. Its historical ledger is preserved through the transition boundary. The modern era continues that same ledger under new state-transition rules. Historical block identities are not rewritten.", "30825f9:doc/design/b3-architecture-contract.md:47-57"),
    ("train", "B3 Hive is the desktop wallet and full-node software for the B3 FlowMesh network. A full node downloads blocks and validates the chain locally. The wallet manages user-controlled keys. These roles belong to the existing B3 application.", "30825f9:README.md:15-27"),
    ("train", "The desktop program is b3coin-qt. The headless full node is b3coind. The RPC client is b3coin-cli. The offline wallet utility is b3coin-wallet. A command name is not proof that its service is currently running.", "30825f9:README.md:15-27"),
    ("train", "The getblockchaininfo command reports chain and synchronization information from the connected node. Its fields include blocks, headers, bestblockhash and initialblockdownload. A lesson cannot tell us the current block height. Current status requires a successful node read.", "30825f9:src/rpc/blockchain.cpp:1385-1400"),
    ("train", "The getnetworkinfo command reports the connected node's network status. The assistant's public projection includes networkactive and connection counts. A network observation describes this node at the time of the read. It does not prove global network health.", "135fc56:src/qt/b3aipublicdata.cpp"),
    ("train", "The getfinalitystatus command reports Modern-PoS finality diagnostics. It can describe configuration, activation, the epoch, the validator set and a finalized checkpoint. Report actual fields and distinguish observations from conclusions. Missing data is not a successful finality check.", "30825f9:src/rpc/blockchain.cpp:3525-3610"),
    ("train", "The listflowmeshmarkets command lists recognized FlowMesh markets through the existing wallet RPC. This read can be unavailable when a wallet cannot be selected. An unavailable result does not mean that there are no markets. An empty successful list and an error are different observations.", "30825f9:src/wallet/rpc/flowmesh.cpp:1087-1108"),
    ("train", "The B3 assistant permits four node reads: getblockchaininfo, getnetworkinfo, getfinalitystatus and listflowmeshmarkets. Each has an empty argument object. The application enforces this allowlist separately from the model. A model's request does not grant permission to call another command.", "135fc56:src/qt/b3aiclient.cpp"),
    ("train", "The assistant cannot spend funds, sign a transaction, place a trade, unlock a wallet or execute shell commands. It can explain information and interpret permitted public diagnostics. Claims that an action succeeded require evidence of that action; explanation alone is not execution.", "135fc56:src/qt/b3aiclient.cpp"),
    ("train", "Public node results are filtered before the assistant sees them. Wallet accounts and balances, proxy addresses, market endpoints, signature-pool details and raw RPC errors are excluded. New fields are not automatically forwarded. Sharing public diagnostics requires the user's tool opt-in.", "135fc56:src/qt/b3aipublicdata.cpp;src/qt/b3aipage.cpp"),
    ("train", "A teaching correction begins as a draft. The teacher checks its answer and source, saves the draft and explicitly approves it. Only approved lessons enter the student's reference context. Saving a draft does not train neural-network weights and does not make the statement true.", "135fc56:src/qt/b3ailessonstore.cpp;src/qt/b3aipage.cpp"),
    ("train", "A lesson's SHA-256 identifier describes its exact content. A hash can help identify a version or detect changed content. It cannot prove that the lesson is correct. Factual correctness requires checking the source and the claim.", "135fc56:src/qt/b3ailessonstore.cpp;README.md"),
    ("train", "A FlowMesh AnchorRef names a B3 block position using a height and a block hash. Existing anchor policy checks the matching block, active-chain membership and required burial depth. This anchor schema contains no lesson text and no language-model weights.", "30825f9:src/flowmesh/deposit.h:21-38;src/node/flowmesh_anchor.cpp:25-33"),
    ("train", "Changing the model connection starts a fresh model conversation. API keys and chat history are kept in memory by the B3 prototype. The endpoint and model name are saved locally. Applying connection settings alone sends no message to a model service.", "135fc56:src/qt/b3aipage.cpp"),
    ("validation", "A disconnected assistant has no current chain observations. Ask the node for its status before describing synchronization. When a read fails, explain that the status is unknown instead of inventing a block height or reporting success.", "135fc56:src/qt/b3aiclient.cpp"),
    ("validation", "A draft correction remains outside the reviewed lesson context. Approval records a review decision. The content identifier detects a particular lesson version; source checking is still needed to establish whether the explanation is accurate.", "135fc56:src/qt/b3ailessonstore.cpp;README.md"),
    ("validation", "Chain anchors refer to blocks. The height and hash identify a chain position, while the policy checks whether that position is accepted. Storing an AI lesson locally does not place its text or its neural weights on the B3 chain.", "30825f9:src/flowmesh/deposit.h:21-38;src/node/flowmesh_anchor.cpp:25-33"),
    ("validation", "A successful market read can return an empty array. A failed market read supplies no evidence about the number of markets. Wallet-related details and private connection fields are removed from the assistant's public diagnostic results.", "135fc56:src/qt/b3aipublicdata.cpp;30825f9:src/wallet/rpc/flowmesh.cpp:1087-1108"),
]


def canonical(record):
    return json.dumps(record, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")


def _record(split, text, source):
    value = {"split": split, "text": text, "source": source, "reviewed": True}
    return {"id": hashlib.sha256(canonical(value)).hexdigest(), **value}


def builtin_curriculum():
    return [_record(*row) for row in _RECORDS]


def corpus_bytes(records):
    return b"".join(canonical(row) + b"\n" for row in records)


def validate_records(records):
    if not isinstance(records, list) or not 2 <= len(records) <= MAX_RECORDS:
        raise ValueError("The corpus must contain 2 to 1024 reviewed records.")
    ids, texts, splits = set(), set(), set()
    for row in records:
        if not isinstance(row, dict) or set(row) != {"id", "split", "text", "source", "reviewed"}:
            raise ValueError("Unsupported corpus record schema.")
        if row["split"] not in ("train", "validation") or row["reviewed"] is not True:
            raise ValueError("Records need an explicit train/validation split and review flag.")
        for key in ("text", "source"):
            if not isinstance(row[key], str) or not row[key].strip() or len(row[key]) > 65536 or "\0" in row[key]:
                raise ValueError("Invalid corpus text or source.")
        expected = _record(row["split"], row["text"], row["source"])["id"]
        if row["id"] != expected or row["id"] in ids:
            raise ValueError("Changed or duplicate corpus record.")
        if row["text"] in texts:
            raise ValueError("Exact duplicate text is prohibited, including across splits.")
        ids.add(row["id"])
        texts.add(row["text"])
        splits.add(row["split"])
    if splits != {"train", "validation"}:
        raise ValueError("Both train and validation records are required.")
    if len(corpus_bytes(records)) > MAX_CORPUS_BYTES:
        raise ValueError("Corpus exceeds the 8 MiB experiment limit.")
    return records


def load_curriculum(path=None):
    if path is None:
        return validate_records(builtin_curriculum())
    with Path(path).open("rb") as stream:
        data = stream.read(MAX_CORPUS_BYTES + 1)
    if len(data) > MAX_CORPUS_BYTES:
        raise ValueError("Corpus exceeds the 8 MiB experiment limit.")
    return validate_records([json.loads(line) for line in data.decode("utf-8").splitlines() if line.strip()])


def split_hash(records, split):
    return hashlib.sha256(corpus_bytes([r for r in records if r["split"] == split])).hexdigest()
