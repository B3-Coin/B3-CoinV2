# FlowMesh operator connection and FN key export RPCs

These commands do not change consensus, committee membership, signing locks,
market history, or settlement. A source build containing this change is required;
do not assume an older preview binary includes the new RPCs.

## Two different keys

| Purpose | Read public identity | Key format | Where it belongs |
| --- | --- | --- | --- |
| Independent peer authentication | `getflowmeshnetworkinfo` → `operator_pubkey` | 66 hex characters, beginning `02` or `03` | Other operators' `flowmeshconnect` targets |
| FN microblock signing | `getflowmeshvalidatorinfo` → `wallet_bls_pubkeys` | 96 hex characters | FN seat signing, not peer connection pins |

Both public identities can be shared. The BLS **secret** described below cannot.
An empty network public key with a startup error is not fixed by substituting
the BLS public key. Diagnose the reported network startup error instead.

## Add an operator peer without restarting

In the local wallet console or authenticated node-admin RPC:

```text
flowmeshconnect "<OTHER_OPERATOR_NETWORK_PUBLIC_KEY>@<REACHABLE_IP>:5649"
getflowmeshnetworkinfo
```

Use the other operator's numeric IPv4 address or bracketed IPv6 address, e.g.
`<PUBLIC_KEY>@[2001:db8::10]:5649` (documentation address, not a working peer).
Hostnames, omitted pins, missing/zero ports, self pins and conflicting target
identities are refused. The existing maximum-peer bound still applies.

The independent FlowMesh service must already be running. This RPC does not
enable the validator service, change its listening address, open a firewall,
forward a router port, or import/arm a signing key. An ordinary engine-off
trading client does not need this operator connection RPC.

The response separates admission from connectivity:

- `accepted: true`, `status: "queued"`: retained for connection attempts, not
  proof that the remote peer received anything.
- `status: "already_present"`: the exact target already exists; no duplicate
  work is created.
- `accepted: false`, `status: "refused"`: inspect `error`; the request was not
  admitted. An unavailable service or invalid JSON type can instead return an
  ordinary RPC error.

`getflowmeshnetworkinfo.targets` exposes per-target `critical`, `action` and
`bulk` channel state, attempt/failure counters, authentication, last error and
retry timing. A last error may remain after recovery; inspect current state
and `authenticated` as well. `retry_in_ms` is a local eligibility delay, not a
promise that connection will succeed at that time. Existing reconnect and
handshake deadlines are unchanged.

Authentication does not prove FN membership, a delivered application message,
quorum, certification or durable application. An unreachable TCP endpoint will
still fail even when RPC admission succeeds. FMN2 remains authenticated
plaintext TCP; this change adds neither transport encryption nor QUIC.

Runtime additions are **memory-only**. To retain a chosen target after restart,
manually add the corresponding line to the existing network-specific config:

```ini
flowmeshconnect=<OTHER_OPERATOR_NETWORK_PUBLIC_KEY>@<REACHABLE_IP>:5649
```

Do not copy placeholders literally. Multiple distinct operators can be added
with repeated RPC calls or config lines. Runtime calls do not edit the file.

## Export one existing FN BLS secret

Select the wallet which owns the FN BLS key, then obtain its complete public key:

```text
getflowmeshvalidatorinfo
```

If encrypted, fully unlock that selected wallet through the normal local
wallet-unlock procedure. Armed staking/FN keys do not make a locked wallet
eligible for export. Then, locally:

```text
exportflowmeshkey "<COMPLETE_96_HEX_FN_BLS_PUBLIC_KEY>" true
```

The required `true` acknowledges the stated risks; it is not a safety check or
proof of a safe signer migration. The response contains `bls_pubkey`, the
sensitive 32-byte `blssecret`, and a warning. Exactly the selected existing key
is returned. The command does not create a key or export the whole wallet.
Watch-only wallets, locked encrypted wallets and wallets without that key are
refused. With multiple wallets, use the selected Qt wallet or explicit
`-rpcwallet=<wallet-name>` in the CLI.

**Do not post the response, screenshots, private key, passphrase or wallet file
in chat/Discord.** The console response contains plaintext. Filtering command
arguments from Qt console history does not hide that response. Terminal
scrollback, clipboard tools and RPC clients can retain it. Never expose wallet
admin RPC publicly or send this secret as a shell command-line argument.

The existing `importflowmeshkey` accepts that secret in a destination wallet.
It stores the key under that wallet's normal encryption state; an unencrypted
wallet does not become encrypted by importing a key. Importing is not the same
as binding an FN seat, obtaining quorum or safely moving an active signer.

## VPS and migration boundary

A BLS-only host does not receive the ordinary owner spending keys. However,
this FN BLS secret authorizes validator votes **and historical FN reward
claims**. Compromising that host can compromise those authorities. This is not
reward-protected delegation or trustless hosting.

Export does not include, migrate, reset or repair signing journals, retained
candidates or certified history. Before moving an existing seat, separately
verify the required persisted history and establish exclusive signer ownership
with the old signer stopped. Never start a second signing runtime with the same
key to test connectivity. A pre-existing `signing-conflict` remains a separate
blocking condition and must not be cleared by deleting its history.

These are local node/wallet-admin RPCs. They are not methods on the public
HTTPS ordinary-trading backend. No live migration, port exposure or key export
is performed merely by building this source change.
