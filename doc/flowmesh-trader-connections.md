# Ordinary FlowMesh trader connections

Ordinary Qt traders use a restricted **HTTPS trading endpoint**. They do not
need an FN seat, a BLS key, an incoming port, or the local validator engine.
`enableflowmeshvalidator` remains false by default.

`flowmeshconnect` is different: it connects the independent **operator TCP
network**, normally on port5649. An operator public key/address is not an
HTTPS endpoint and must not be pasted into the trader connection field.

## Connect and reconnect

Use the Trade page's HTTPS connection field, or select a wallet in the Qt
console and run:

```text
flowmeshclientconnect "https://YOUR-APPROVED-TRADING-HOST"
getflowmeshclientinfo
```

The hostname above is a placeholder, not a deployed service. Use the actual
HTTPS origin provided by the operator. This command changes connection
configuration and performs a read-only check. It does not unlock the wallet,
arm a validator, create an order, or retry a signed instruction.

`reconnectflowmeshclient` remains available for a fresh read-only probe of the
existing endpoints. It bypasses automatic-read cooldowns, returns `busy` when
another client request is running, and does not save configuration or retry
signed instructions.

New runtime endpoint URLs are retained for reopening the same datadir. Up to
eight endpoints can be configured for failover. Normal market reads continue
retrying with bounded transport backoff; the Qt Trade page refreshes
automatically. A temporarily unavailable selected endpoint is retried after
its cooldown even when another endpoint is serving reads successfully.
An unavailable server is not a reason to repeatedly submit
an order or create a replacement deposit. Saved requests keep their original
identities and existing explicit recovery rules.

This first connection command has no endpoint-removal operation. The eight
entry limit includes retained runtime endpoints. Probes and reads use the
existing serialized backend; a connection attempt can wait for work already
in progress and try several endpoints, each with its existing five-second
network deadline. Neither Connect nor application shutdown has a guaranteed
five-second total duration.

Runtime connection uses normal certificate and hostname verification. Selecting
an endpoint already supplied at startup preserves its configured CA and pin.
The connection command does not accept a TLS private key, wallet password,
certificate-verification bypass, HTTP URL or credential-bearing URL.

Explicit startup configuration remains supported:

```ini
enableflowmeshvalidator=0
flowmeshendpoint=https://YOUR-APPROVED-TRADING-HOST
```

Private test services may require the existing `flowmeshendpointca` and
`flowmeshendpointpin` options. A pin is additional verification: it never
replaces CA or hostname checking. CA trust-bundle availability must be verified
on each packaged platform, especially Windows; do not disable system or TLS
protections to make a connection work.

## Connection is not market readiness

Keep these observations separate:

- A saved endpoint may not have responded yet.
- A verified HTTPS response proves transport success, not a market certificate.
- A market without its first certificate remains visible but cannot trade.
- Certified data is accepted only after the existing client proof checks.
- A successful request/submission is not a fill or a completed withdrawal.

Failure to read one selected market must not remove successfully discovered
markets. Changing markets does not sign, resubmit or cancel an instruction.

## Public seed deployment

A shipped seed must name a real approved trading service with valid HTTPS
trust and sufficient availability. It must not point to an operator TCP port
or an administrative RPC listener. No public seed is fabricated by this patch.
The existing VPS's loopback-only HTTPS service and a user's private SSH tunnel
are not externally usable tester endpoints.

This change retries saved/configured trading endpoints. It does not introduce
unauthenticated peer-gossip endpoint discovery, change FN membership, or turn
the operator network into encrypted transport. Operator FMN2 remains
authenticated plaintext TCP. A public VPS seed still requires its actual
hostname/certificate/access configuration and separate connection checks.
