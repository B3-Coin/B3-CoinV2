# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Intentionally narrow, teacher-authored B3 dialogue training targets.

Each reviewed topic has one concise answer and independently worded questions
of different lengths. These simulated turns contain no live node observations
or user chats. Validation asks new questions about the same known facts: it
checks paraphrase selection, not independent knowledge or task competence.
Targets are training data; inference must generate from weights, not look up
these answers. Short records are intentional because teach.py uses full turns.
"""

import argparse
from pathlib import Path

from .data import _record, corpus_bytes, validate_records


GREETING = "teacher-authored;135fc56:src/qt/b3aipage.cpp"
HIVE = "teacher-authored;30825f9:README.md:15-27"
CHAIN = "teacher-authored;30825f9:doc/design/b3-architecture-contract.md:47-57"
LESSON = "teacher-authored;135fc56:src/qt/b3ailessonstore.cpp"
MEMORY = "teacher-authored;135fc56:src/qt/b3ailessonstore.cpp;src/qt/b3aipage.cpp"
HASH_TRUTH = "teacher-authored;135fc56:src/qt/b3ailessonstore.cpp;README.md"
TOOLS = "teacher-authored;135fc56:src/qt/b3aiclient.cpp"
PUBLIC = "teacher-authored;135fc56:src/qt/b3aipublicdata.cpp"
CONNECTION = "teacher-authored;135fc56:src/qt/b3aipage.cpp"
BLOCK = "teacher-authored;30825f9:src/rpc/blockchain.cpp:getblockchaininfo"
FINALITY = "teacher-authored;30825f9:src/rpc/blockchain.cpp:getfinalitystatus"
NETWORK = "teacher-authored;30825f9:src/rpc/net.cpp:getnetworkinfo"
MARKET = "teacher-authored;30825f9:src/wallet/rpc/flowmesh.cpp:listflowmeshmarkets"
ANCHOR = "teacher-authored;30825f9:src/flowmesh/deposit.h:21-38"


# Topic, canonical answer, reviewed source, eight authored training questions,
# one new validation question. This balanced repetition is deliberate narrow
# supervised practice, not a general language foundation or inference router.
_TOPICS = [
    ("greeting", "Hello! Let's learn B3.", GREETING, (
        "Hello",
        "Hi",
        "Hey there",
        "Good morning",
        "Hello, student",
        "Can we start learning B3?",
        "I would like a friendly greeting before my B3 question.",
        "Good evening, I am ready to learn about the B3 project.",
    ), "Greetings, B3 student."),

    ("continuous_chain", "B3 is one continuous blockchain with preserved historical identity.", CHAIN, (
        "What is B3?",
        "Explain B3 briefly.",
        "Is B3 one continuous chain?",
        "Does B3 preserve its blockchain history?",
        "Is modern B3 a new and unrelated blockchain?",
        "How are the legacy and modern B3 ledgers related?",
        "When B3 transitions to modern rules, does its historical identity remain?",
        "Tell me whether B3 keeps the same historical ledger through its transition.",
    ), "How would you define B3?"),

    ("hive_purpose", "B3 Hive is B3's desktop wallet and full-node software.", HIVE, (
        "What is Hive?",
        "What is B3 Hive?",
        "Explain the purpose of Hive.",
        "Which roles does the B3 Hive application combine?",
        "Is B3 Hive a desktop wallet and a full node?",
        "What kind of software is the Hive application for B3?",
        "I am new to B3; describe what the Hive desktop application provides.",
        "What are the wallet and node roles of the application called B3 Hive?",
    ), "Describe Hive's main role."),

    ("desktop_program", "b3coin-qt is the B3 Hive desktop wallet.", HIVE, (
        "Name the desktop wallet.",
        "Which program opens Hive?",
        "What is b3coin-qt?",
        "Which executable has the B3 graphical wallet?",
        "How is the B3 Hive desktop program named?",
        "I want the graphical B3 wallet; which program is that?",
        "Tell me the executable name associated with the Hive desktop wallet.",
        "Which B3 program provides the desktop interface rather than the headless node?",
    ), "Name the graphical Hive executable."),

    ("headless_program", "b3coind is the headless B3 full node.", HIVE, (
        "Name the headless node.",
        "What is b3coind?",
        "Which program runs B3 without a GUI?",
        "What is the B3 full-node daemon called?",
        "Is there a B3 full node without a desktop window?",
        "I want the full node without the graphical wallet; name its program.",
        "Which B3 executable is intended to run as a headless full node?",
        "What should I call the node program that has no Hive desktop interface?",
    ), "Which executable is B3's daemon?"),

    ("rpc_client", "b3coin-cli is B3's RPC command-line client.", HIVE, (
        "Name the RPC client.",
        "What is b3coin-cli?",
        "Which B3 program is the command-line client?",
        "What is B3's command-line RPC client called?",
        "Which executable sends B3 RPC commands from a terminal?",
        "Give me the name of the B3 program used as an RPC client.",
        "I need the command-line client for the B3 RPC interface; which program is it?",
        "Which listed B3 executable serves as the RPC client rather than the full node?",
    ), "Identify B3's terminal RPC client."),

    ("offline_wallet", "b3coin-wallet is B3's offline wallet utility.", HIVE, (
        "Name the offline wallet utility.",
        "What is b3coin-wallet?",
        "Which B3 utility works with wallets offline?",
        "What is the offline B3 wallet program called?",
        "Which listed executable is the offline wallet utility?",
        "Tell me the program name for B3's offline wallet utility.",
        "I am looking at B3's program list; which entry is the offline wallet utility?",
        "How is the offline wallet utility named, separately from the Hive desktop program?",
    ), "Identify B3's offline wallet program."),

    ("draft_exclusion", "Saved draft lessons stay outside reference context.", LESSON, (
        "Can drafts teach yet?",
        "Are draft lessons in context?",
        "What happens to a new draft lesson?",
        "Does an unapproved lesson enter reference context?",
        "Can saving a draft alone add it to the student's context?",
        "I saved a correction but did not approve it; is it used as reference?",
        "Where does a saved lesson stand before the explicit approval step is completed?",
        "Should the student retrieve a custom lesson that is still marked as a draft?",
    ), "Can unapproved drafts become references?"),

    ("explicit_approval", "Explicit teacher approval lets a saved lesson enter reference context.", MEMORY, (
        "How does a lesson become approved?",
        "When may a saved lesson enter context?",
        "What enables a draft lesson for retrieval?",
        "Which step lets a saved correction become reference material?",
        "Do I need to approve a lesson after saving its draft?",
        "After checking the answer and source, how do I make a saved lesson available?",
        "What review action must the teacher take before the student can retrieve a draft?",
        "I want the student to use a checked correction; what approval step is required?",
    ), "What makes saved corrections retrievable?"),

    ("local_memory", "Local lessons are reference memory, not weight training or on-chain data.", MEMORY, (
        "What are local memories?",
        "Does saving lessons train weights?",
        "Are saved lessons published on B3?",
        "What kind of memory does the lesson file provide?",
        "Is local lesson storage the same as neural-network training?",
        "Does approving a stored reference lesson put its content on the blockchain?",
        "When I save a B3 teaching correction, am I updating model weights or local memory?",
        "Explain whether the lesson store provides reference memory or an on-chain learning record.",
    ), "Are stored lessons weight updates?"),

    ("hash_identity", "A SHA-256 lesson ID identifies its canonical content version.", LESSON, (
        "What does a lesson ID mean?",
        "What is the lesson hash for?",
        "What does SHA-256 identify for a lesson?",
        "How does the store identify a lesson's content version?",
        "What is represented by the SHA-256 identifier on a saved lesson?",
        "Why does the lesson store give each canonical content version a hash?",
        "Tell me what a lesson's identifier describes after its content is canonicalized.",
        "What does the SHA-256 lesson identifier refer to when I review a saved correction?",
    ), "What version does SHA-256 identify?"),

    ("hash_truth", "A content hash identifies content; it does not prove factual truth.", HASH_TRUTH, (
        "Does a hash prove truth?",
        "Is a matching lesson hash proof of correctness?",
        "Can SHA-256 prove that a lesson is true?",
        "Does agreement on a lesson hash establish factual accuracy?",
        "If the lesson hash matches, can its answer still be wrong?",
        "Can a content identifier replace checking a lesson's claim against its source?",
        "Two reviewers have the same lesson hash; does that establish that the statement is true?",
        "Explain whether hashing an answer is enough to demonstrate its factual correctness.",
    ), "Do matching hashes establish factual reliability?"),

    ("builtin_seeds", "Built-in source lessons are immutable; custom lessons are separate.", LESSON, (
        "Can I delete the built-in lessons?",
        "Are source seeds editable?",
        "Can custom saves replace built-in seeds?",
        "Does the custom lesson file contain mutable built-in lessons?",
        "May a teacher remove the built-in source lessons through the store?",
        "Are the built-in reference seeds changed when I save a custom lesson?",
        "How does the lesson store distinguish immutable source seeds from custom corrections?",
        "Can editing my local custom lessons overwrite the application's built-in reference seeds?",
    ), "Are the built-in source references part of the editable custom lesson file?"),

    ("chain_diagnostic", "getblockchaininfo reports this node's chain and synchronization status.", BLOCK, (
        "Which read reports chain sync?",
        "What does getblockchaininfo do?",
        "Name the chain-status diagnostic.",
        "Which command supplies chain synchronization information?",
        "Where do I read this node's validated height and best block hash?",
        "Which diagnostic should supply observations about the node's chain progress?",
        "For a chain synchronization explanation, which read command provides the node's status?",
        "Which permitted diagnostic reports the connected node's blockchain processing state?",
    ), "What read command gives this node's chain information and initial-sync status?"),

    ("network_diagnostic", "getnetworkinfo reports this node's network status and connection counts.", NETWORK, (
        "Which read reports connections?",
        "What does getnetworkinfo do?",
        "Name the network-status diagnostic.",
        "Which read supplies this node's network connection counts?",
        "Where can I read whether this node's network is active?",
        "Which diagnostic provides network observations for the connected node?",
        "I want this node's connection totals rather than global network guesses; which read fits?",
        "Which allowed command reports this node's network status and numbers of connections?",
    ), "What command would supply network and connection information for this node?"),

    ("finality_diagnostic", "getfinalitystatus reports this node's Modern-PoS finality diagnostics.", FINALITY, (
        "Which read reports finality?",
        "What does getfinalitystatus do?",
        "Name the Modern-PoS diagnostic.",
        "Which command describes this node's finality state?",
        "Where can I read Modern-PoS activation and finalized checkpoints?",
        "Which diagnostic reports the current validator set and finality checkpoint?",
        "I need node-level Modern-PoS finality observations; which permitted read is relevant?",
        "What read supplies finality diagnostics instead of an unsupported guess about the node?",
    ), "Which diagnostic can report this node's modern finality configuration and epoch state?"),

    ("market_diagnostic", "listflowmeshmarkets lists recognized markets; errors are not empty lists.", MARKET, (
        "What does listflowmeshmarkets do?",
        "Which read lists FlowMesh markets?",
        "Does a market-read error mean zero markets?",
        "Name the market diagnostic and distinguish failure from emptiness.",
        "If listing FlowMesh markets fails, is that the same as an empty list?",
        "Which read lists recognized markets, and can an error establish there are none?",
        "Describe the market-list diagnostic without treating an unavailable result as an empty array.",
        "What is the recognized-market read, and how should I interpret a failed call to it?",
    ), "Does the recognized-market listing command prove an empty market set when it fails?"),

    ("unknown_live_status", "Current node status is unknown; no live read has been run.", TOOLS, (
        "Check node status",
        "Is my node synced?",
        "What is my current block height?",
        "Tell me the current node status without any tool results.",
        "No diagnostic has run; can you report my live synchronization state?",
        "I have supplied no node observations; what can you say about its current status?",
        "Without a successful live read, do we know whether my B3 node is synchronized?",
        "Please check the node, but this simulated conversation has no executed diagnostic tools.",
    ), "Can you establish my node's current health when no live diagnostic result is available?"),

    ("tool_arguments", "Only four diagnostic reads with an empty JSON object are permitted.", TOOLS, (
        "Can you call any RPC?",
        "What arguments do node tools accept?",
        "Are arbitrary node commands allowed?",
        "Can a model supply extra arguments to a permitted read?",
        "How does the assistant limit RPC names and argument values?",
        "Does a model request allow commands beyond the four diagnostic reads?",
        "What restriction applies to the assistant's permitted node calls and their arguments?",
        "May a lesson expand the RPC allowlist or add parameters to the diagnostic commands?",
    ), "Can the assistant use extra RPC methods or nonempty arguments just because a model asks?"),

    ("no_write_tools", "No spending, signing, trading, wallet unlocking or shell tools are offered.", TOOLS, (
        "Send my B3 coins.",
        "Sign a transaction for me.",
        "Unlock my wallet.",
        "Run a shell command.",
        "Place a FlowMesh trade for me.",
        "Can the assistant spend coins or create a signed transaction through its tools?",
        "Are transaction signing, wallet unlocking and shell execution available to this student?",
        "Which write operations can this assistant perform with the current diagnostic-only tools?",
    ), "Does this assistant have tools that execute trades, transactions or shell commands?"),

    ("public_projection", "Public projections exclude wallet balances, accounts and proxy addresses.", PUBLIC, (
        "Are wallet balances sent to the model?",
        "Do public reads reveal account IDs?",
        "Are proxy addresses included in model context?",
        "What private fields are removed from public diagnostic results?",
        "Does the assistant's market projection include my wallet accounts and balances?",
        "Does a public network projection forward my configured proxy address to the model?",
        "Explain whether wallet account data and proxy addresses are shared in public node results.",
        "When a diagnostic result reaches the model, are local wallet accounts or balances retained?",
    ), "Can the model's public diagnostic context include wallet balances or configured proxies?"),

    ("node_consent", "Node tools start off and require explicit opt-in before public reads.", CONNECTION, (
        "Are node tools on by default?",
        "Must I enable node tools?",
        "Does opening Hive enable diagnostic sharing?",
        "Is public node access enabled automatically when the page opens?",
        "What consent is required before the assistant can use node reads?",
        "Can the model request public diagnostics before I opt in to node tools?",
        "How does the assistant begin each startup with respect to node-tool permission?",
        "Does configuring the model service automatically grant permission to read public node status?",
    ), "Do public diagnostic tools become available at startup without explicit user consent?"),

    ("connection_transport", "Remote model services require HTTPS; HTTP is limited to loopback IPs.", TOOLS, (
        "Can a remote model use HTTP?",
        "When is HTTP allowed?",
        "Does the model URL need HTTPS?",
        "What transport is required for a remote model service?",
        "Can an unencrypted model connection use an ordinary remote hostname?",
        "Where may the assistant use an HTTP endpoint instead of an HTTPS endpoint?",
        "Explain the allowed model-service transport for remote hosts and literal loopback IP addresses.",
        "Is unencrypted HTTP permitted for a model outside this computer's loopback addresses?",
    ), "Which model-service URLs may use HTTP, and what must a remote service use?"),

    ("flowmesh_anchor", "FlowMesh AnchorRef identifies a B3 block by height and hash, not lessons.", ANCHOR, (
        "What is a FlowMesh anchor?",
        "Does AnchorRef store lessons?",
        "Are neural weights in AnchorRef?",
        "What data identifies a FlowMesh chain anchor?",
        "Is the existing AnchorRef schema a place for learning text?",
        "Does a FlowMesh block anchor contain a lesson answer or model weights?",
        "Describe the height-and-hash reference used by FlowMesh without treating it as AI storage.",
        "What does FlowMesh AnchorRef identify, and is it an on-chain container for student lessons?",
    ), "Is the existing FlowMesh anchor a reference to a block or a stored AI lesson?"),
]


def builtin_dialogues():
    """Return fresh records; validation rephrases the same reviewed topic facts."""
    records = []
    for _, answer, source, questions, held_out_question in _TOPICS:
        for question in questions:
            records.append(_record("train", f"\nUser: {question}\nAssistant: {answer}\n", source))
        records.append(_record("validation", f"\nUser: {held_out_question}\nAssistant: {answer}\n", source))
    return validate_records(records)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="New reviewed dialogue JSONL file; existing files are refused")
    args = parser.parse_args(argv)
    try:
        records = builtin_dialogues()
        with args.output.open("xb") as stream:
            stream.write(corpus_bytes(records))
    except (OSError, ValueError, TypeError) as error:
        parser.error(str(error))
    train_count = sum(row["split"] == "train" for row in records)
    print(f"Wrote {train_count} training and {len(records) - train_count} validation dialogues.")
    print("Narrow teacher-authored targets; validation rephrases known facts, not independent task competence.")


if __name__ == "__main__":
    main()
