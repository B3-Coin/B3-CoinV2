// Run with: node --experimental-strip-types --test web/test/
import assert from "node:assert/strict";
import { test } from "node:test";

import { accountText, assetName, atoms, isHash, liveText, statusText, totalsNote, type V2Info } from "../src/v2fmt.ts";

const NBSP = " ";
const Q = "51".repeat(32);

const info: V2Info = {
  mode: "regtest-pbft-view-change",
  seat: 0,
  seats: 4,
  quorum: 3,
  height: 12,
  head_hash: "00".repeat(32),
  state_root: "00".repeat(32),
  certified: true,
  pending_actions: 1,
  mainnet_enabled: false,
  assets: [{ name: "Q", id: Q }],
  markets: [],
};

test("atom counts are grouped exactly and never parsed as numbers", () => {
  assert.equal(atoms("0"), "0");
  assert.equal(atoms("1000"), `1${NBSP}000`);
  assert.equal(atoms("123456789012345678901234567890"), ["123", "456", "789", "012", "345", "678", "901", "234", "567", "890"].join(NBSP));
  assert.equal(atoms("-5"), "-5");
});

test("assets are named from the operator's list or shown shortened", () => {
  assert.equal(assetName(info, Q), "Q");
  assert.equal(assetName(info, "ab".repeat(32)), "abababab…ababab");
  assert.equal(assetName(null, Q), "51515151…515151");
});

test("the status names the seat, the quorum and whether the head is certified", () => {
  assert.equal(statusText(info), "regtest-pbft-view-change, seat 0 of 4, quorum 3: certified height 12, 1 pending action.");
  assert.match(statusText({ ...info, certified: false, height: 0, pending_actions: 2 }), /genesis, no certified state yet, 2 pending actions/);
});

test("the live line marks data older than 3 s as stale", () => {
  assert.equal(liveText(null, null, 0, null), "Waiting for the certified-state stream.");
  assert.equal(liveText(7, 1000, 3000, null), "Certified state 7, received 2 s ago.");
  assert.match(liveText(7, 1000, 4001, null), /Stale/);
  assert.match(liveText(7, 1000, 1000, "refused"), /failed and is retrying: refused/);
});

test("totals and accounts say what the operator reported", () => {
  assert.match(totalsNote({ height: 3, certified: true, conservation: true, assets: [] }), /certified height 3\. The operator reports/);
  assert.match(totalsNote({ height: 0, certified: false, conservation: false, assets: [] }), /genesis state\. The operator does not report/);
  assert.equal(accountText({ account: Q, found: false, height: 4, certified: true, operator_verified: false, balances: [], next_nonce: null }), "No such account at height 4.");
  assert.equal(
    accountText({ account: Q, found: true, height: 4, certified: true, operator_verified: true, balances: [], next_nonce: "3" }),
    "Account at height 4: next nonce 3; the operator checked its proof against the certified head.",
  );
  assert.ok(isHash(Q) && isHash("ab".repeat(32)) && !isHash("AB".repeat(32)) && !isHash("51"));
});
