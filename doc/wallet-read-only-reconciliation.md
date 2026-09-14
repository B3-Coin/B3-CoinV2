# Read-only wallet / UTXO comparison

This diagnostic does not migrate, rescan, repair, sign or send. It uses the
existing `scantxoutset` validated-chain cursor, not another chainstate. It
requests public wallet descriptors (`private=false`), including P2PK through
`combo`/`pk` descriptors. Address-only scans do not cover historical P2PK.
Already-known STAKE outputs use their exact full scripts and the existing
`getstakinginfo` owner classification. Validator keys are not spending keys.

The new tool is bounded and deliberately **not a complete recovery engine**.
Unknown STAKE/asset wrappers, unknown descriptor ranges and already-spent
history are not discovered. Nonstandard/asset scripts remain unclassified;
native carrier value is never labelled a colored-asset balance. It does not
prove key possession or current authorization from a matching script. Reward
maturity requires the current chain's rules, not a hard-coded Bitcoin value.

`gettxout` can report an ordinary owner-script type for a B3A1 or STAKE
wrapper. The diagnostic therefore checks the **entire script**, not that
type alone, before assigning standard native-B3 value: exact bare P2PK
(compressed/uncompressed), P2PKH, P2SH, witness-v0 key/script hash and
witness-v1 taproot forms are supported. Unrecognized wrappers and other
forms, including bare multisig, remain unclassified; their full scripts and
native carrier values are retained. Separately recognized, already-known
STAKE records retain their existing owner/activation evidence only when a
current coin matches the exact observed full script, canonical STAKE prefix,
supported bare P2PK/P2PKH owner suffix, validator identity and positive amount.
Other owner forms remain unclassified; this is not a second consensus parser.
A historical STAKE row alone is not a current
balance. No current coin means no current value. This does not
decode colored-asset amounts or infer spending authority.

## Short recovery checklist

1. Keep the original wallet and signing history untouched. Record exact build,
   wallet format, public chain height/hash and the complete error. Database
   loading, migration, history scanning, missing keys, and chain sync are
   different problems; a wallet error does not invalidate historical coins.
2. Obtain a supported, verified consistent backup before any separately
   approved migration. Never copy an actively written database casually.
3. Compare public wallet scripts against the validated UTXOs at one recorded
   height **and hash**. Keep reports private: they reveal financial history.
   Match exact outpoints and integer units, not symbol/address totals.
4. Treat mempool spends, immature rewards, user coin locks and watch-only
   authority separately. A STAKE activation delay is not spend maturity.
5. If history is missing, first establish correct key/script conversion and
   availability of the entire intended block range. A wrong birthday must not
   exclude earlier history. Pruned/missing blocks mean incomplete evidence,
   not a successful zero balance. The existing `rescanblockchain`/abort path
   is a separate, expressly approved operation—not run by this tool.
6. Compare before/after at the same chain target. Never change historical
   timestamps, transaction IDs or spent outpoints to manufacture a balance.

## Result and exit status

- `0`: a bounded comparison completed, **not** complete wallet recovery.
- `2`: a report was written but the comparison is `INCONCLUSIVE`; inspect its
  explicit reasons (including an active wallet rescan, the recent-history
  limit, changing chain/mempool, unsupported policy or inconsistent STAKE data).
- `1`: the operation failed or was interrupted; the protected report records
  `INCOMPLETE` where possible. It does not imply a zero balance.

The recent-history limit only covers recent entries, not the complete wallet.
Even exit 0, equal totals or matching UTXOs cannot establish that all transactions,
keys or derivations have been recovered. `gettxout` does not expose legacy
coinstake status: a false coinbase field does not prove an ordinary/mature coin.

## Explicit invocation (disposable regtest example)

```
python3 contrib/wallet-reconcile.py --cli /absolute/test-build/bin/b3coin-cli \
  --datadir /absolute/disposable-regtest --wallet generated-test \
  --network regtest --output /private/test-report.json
```

No default wallet/datadir is used; RPC must already be running. The tool does
not start a node. Do not run it on a real wallet under this task's authority.
Existing-wallet use requires separate authorization. Reports are exclusive
0600 files; existing files are never overwritten. Raw secrets/private
descriptors are not requested, and RPC error text is omitted from reports.

The default per-descriptor range cap is 1,000 entries; exceeding it fails
explicitly rather than truncating a successful scan. Increase only to a known
required range and record it. Recent wallet history and matches are bounded.
The node scan supports `scantxoutset status` and `abort`; do not abort someone
else's scan. A client deadline does not automatically cancel a node's RPC
worker. An interrupted run writes INCOMPLETE; inspect scan ownership/status.
The output-count limit is checked after the node returns its scan/list replies;
it is not a server-side CPU/memory or UTXO-walk limit. This first tool therefore
does not claim a fully bounded or automatically cancelled node scan. Use the
existing scan progress/abort interface only after establishing scan ownership.

The CLI uses one explicit network selector and verifies the RPC node's reported
chain. It does not edit the node's configuration. Empty successful `gettxout`
output represents the CLI's null result, not a parsing failure or a zero-value
coin. Empty responses from other methods still fail. Failure reports include
the method and numeric RPC code where available, but not remote error text or
authenticated URLs.

RPC parameters (including public descriptors) are passed through the CLI's
standard input, not its process argument list. Reports remain private local
ownership evidence; do not publish them or distribute generated test wallets.
The diagnostic never automatically aborts a node scan. A timeout is not proof
that the server stopped: inspect ownership before any separately approved abort,
and never cancel a different user's scan just to retry this tool.

Sequential RPC snapshots are checked before/after and against each coin's
bestblock. A changed chain makes the report INCONCLUSIVE. This is not an
atomic snapshot: a leave-and-return reorg can escape endpoint comparison.
Mempool sequence changes are reported separately. Matching current UTXOs
cannot reconstruct spent history, labels, absent keys or unknown derivations.
