# Milestone 3A: disk-backed TEST recovery contract

Baseline: `eb73e12d8b042f3ccdfd1e0d96701460e07f20d5`, on `flowmeshV2-dev`.
This document precedes implementation. It changes no agreement, accounting,
authentication, membership, anchor or economic rule. All identities, signatures,
anchors and custody remain the existing synthetic model facilities.

## Required durability and evidence

1. Before the synthetic signer is called: persist the exact phase/view/instance
   intent together with its selected candidate body. Persist accepted proposal,
   matching NEW_VIEW and prepared/highest-prepared evidence using the existing
   transition order. An intent reserves its existing voting slot; a partial
   filesystem operation does not authorize another value.
2. Before publication: persist the exact returned signed object. Recovery may
   reissue an unsigned intent only through existing evidence/mode predicates,
   or retransmit an already stored signature unchanged. No new sequence,
   payload, lock reset or assumed certificate absence.
3. Persist a verified decision certificate and exact body before accounting.
   The resulting accounting snapshot, applied/count marker, parent, anchor,
   sequence and next record form one atomic transaction. A crash exposes the
   previous state or the entire new state, never a partial application.
4. Missing volatile ancestry is fetched using the existing GET/DATA path.
   Missing/corrupt protected records are a storage refusal, not a request to
   erase obligations or initialize a replacement store.

## Storage design and failure boundary

Use Python's SQLite adapter (SQLite atomic transactions, rollback journal,
`synchronous=FULL`) rather than a new production journal protocol. Versioned
typed JSON preserves integer and tuple keys and bytes without pickle. Store
identity binds the generated signer index, fixed configuration and genesis.
An exclusive OS advisory lock is held for the full process lifetime; never
unlink its lock file. Every reopening requires the existing store unless the
test explicitly provisions a fresh generated directory.

Keep a checksummed, hash-chained change journal and materialized state in the
same database transaction. Recovery verifies format, identity, journal chain,
materialization agreement and required record structure. Bounds stop writes
without pruning obligations. Changes to touched records are persisted; network
DATA/OFFER caches are not promoted into permanent storage by the adapter.

Test hooks cover transaction entry, row writes, pre-commit, post-commit and an
explicit post-commit file/directory fsync barrier. SQLite itself controls its
internal flushes; injected failures at our barrier are not tests of a custom
SQLite VFS or actual device write-cache failure. Any write/flush failure makes
the running adapter unavailable. No signature/publication continues based on
an unacknowledged write. Reopen reconciles the original committed identity.

Kill child processes at named replica and storage boundaries, capture actual
exit codes, and reopen the same directories. Test helpers never arm or access
real wallets, keys, nodes, contracts or services. The parent synthetic signer
oracle records all issued signatures, including ones never published, for the
unchanged independent safety checker.

## Rollback and ownership assumptions

The parent test harness may retain a trusted generation/digest observation
outside a child's store. A store older than that observation, or mismatching
it, must refuse signing. Checksums alone do not detect a coherently restored
older backup. Without that external observation, rollback remains undetectable;
this is an explicit production gap, not an automatically recoverable case.
Advisory locking covers cooperating processes on one local filesystem, not
cloned stores, hostile owners or multiple machines using the same real key.

## Required cases and limits

A: before intent durability; B: after signature durability before publication;
C: after publication before certificate; D: prepared before view-change
completion; E: decision before application; F: application transaction and
marker; G: application durable before shutdown. Add malformed/truncated rows,
write/fsync failures, unavailable storage, inconsistent journal/state, duplicate
ownership, known rollback and same-object retry/exactly-once replay cases.

Retain bounded DATA/OFFER admission and retry checks through this adapter.
New storage tests supplement all 237 baseline tests. They prove only bounded
process-kill and injected-I/O scenarios on the tested local filesystem, not
machine power loss, real BLS, B3 evidence, membership changes, bridge validity,
futures margin, live V1 recovery or WAN/200 ms performance.

## Separate review gate

The final DATA/OFFER/retry implementation is still pinned for review at the
published baseline above. The prior independent review tool restriction is
not retried or bypassed here. No separately authorized implementation reviewer
is currently exposed by the tool catalog. External reviewers can use the
commit-pinned map in `doc/design/v2-milestone-2-1-publication.md` at that revision.
Storage development may proceed; its integrated safety qualification remains
conditional on closure of that review and any findings affecting assumptions.
Local and hosted tests are not that missing review. No push or next milestone
before review of this completed local milestone.
