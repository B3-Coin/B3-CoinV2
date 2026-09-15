# Historical block-sync stall observed at height 344404

## Cause

A fresh historical sync followed a stale legacy peer to height 344404. Modern
archival peers had different later history. The active tip's sparse locator
matched their shared history at height 82251, so they returned the 500 known
blocks at heights 82252 through 82751. All 500 hashes matched locally stored
history.

The downloader skipped these known blocks and requested the next page using
the **same active-tip locator**. The peer therefore returned the same page
again. Changing the download owner did not advance discovery. A downloaded
side branch also needs discovery progress before it has enough history to
become the active chain.

This is a generic historical discovery bug; no height-specific exception or
new chain checkpoint is introduced. A connected peer or an advertised height
is not evidence of successful synchronization.

## Repair

- Keep one bounded, per-peer discovery cursor independent of the active tip.
  Continue from available linked history, including a stored side branch.
- Advance the cursor through the available prefix of a requested inventory
  page. Never jump over a missing body to a known suffix. An index alone is
  not enough: body availability, transaction-validation status and linked
  transaction history are required.
- Advance from requested full blocks only after the existing processing and
  storage path. Cursor eligibility does not establish full consensus validity
  of an inactive branch; normal connection checks still apply.
- Recheck cursor availability before use. Release repeated/nonadvancing pages
  instead of spinning; allow rebasing after the existing peer cooldown.
- Retain per-peer progress across ordinary owner lease changes. Known inventory
  does not extend the lease. Only the owner can issue legacy download requests,
  including when a former owner delivers a late requested block.

The 32-block download window, inventory limits, retry/lease timing, block
validation, H/X anchor, finality, quorum and signing journals are unchanged.
Legacy 3.x software rejection remains a post-boundary policy; this change does
not introduce an IP ban or alter historical consensus.

## Regression scope

The focused `legacy_net_tests/legacy_discovery*` cases exercise real mocked P2P
INV/GETBLOCKS/GETDATA handling with isolated synthetic block-index histories.
They do not use a live wallet, validator or community database. The original
five-case regression group fails against the pre-fix implementation: known-page
advance, stored side-branch continuation, missing-body handling and retained
peer-local progress fail; the bounded unknown-block request window passes.

All seven focused regressions pass with the repair, including a received
side-branch body, late former-owner response, and invalid-body/known-suffix
case. Synthetic fixture candidate-set/header bookkeeping and locked status
inspection were corrected during test development; the normal block-index
and locking assertions remain enabled. These fixture failures are not claimed
as production failures or repairs.

The related obsolete-3.x policy test had a separate, pre-existing mock-handshake
defect: manual peers never sent their initial VERSION, and the shared mock
transport was not drained before injecting VERACK. The unchanged production
implementation reproduces that failure. Correcting the fixture sequencing
passes all its original assertions; no peer-policy assertion was weakened and
no production rejection rule was changed.

The selected networking, modern-orphan, boundary, checkpoint, identity and
legacy-orphanage groups pass all 48 cases. Full transition-suite qualification
is not claimed: its existing `load_external_block_file_uses_legacy_codec`
fixture aborts while destroying an unclosed bootstrap writer, before calling
the importer. That separate fixture and the cases aborted after it are not
repaired or counted as passes in this change.

This documents the source repair, not recovery of a deployed installation.
Deployment requires its own controlled restart and verification of sustained
tip advancement toward the existing sealed history; no reindex, chain
invalidation or signing-state reset is part of the repair.
