// Run with: node --experimental-strip-types --test web/test/
import assert from "node:assert/strict";
import { test } from "node:test";

import { BLOCK_SECONDS, LONG_WAIT_SECONDS, outlook, shareText, stakeOutlook, waitText } from "../src/outlook.ts";

const G = " ";

test("a validator's share of blocks is its share of the counting weight", () => {
  const o = outlook(100, 10_000);
  assert.ok(o);
  assert.equal(o.share, 0.01);
  assert.equal(o.blocksBetween, 100);
  assert.equal(o.secondsBetween, 100 * BLOCK_SECONDS);
  // The whole network: every block.
  assert.deepEqual(outlook(50, 50), { share: 1, blocksBetween: 1, secondsBetween: BLOCK_SECONDS });
  // Nothing counting, or nothing to count against, says nothing.
  assert.equal(outlook(0, 10_000), null);
  assert.equal(outlook(10, 0), null);
  assert.equal(outlook(Number.NaN, 10), null);
  // A weight above the total (a stale read) is capped.
  assert.equal(outlook(20, 10)?.share, 1);
});

test("a new stake joins both this validator's weight and the network's", () => {
  // 10 B3 into a network of 990: one block in 100.
  assert.equal(stakeOutlook("10", 0, 990)?.blocksBetween, 100);
  // Added to an existing 90 B3 of own weight.
  assert.equal(stakeOutlook("10", 90, 990)?.blocksBetween, 10);
  // Weight counts in whole B3: half a coin adds nothing.
  assert.equal(stakeOutlook("0.5", 0, 990), null);
  assert.equal(stakeOutlook("", 0, 990), null);
  assert.equal(stakeOutlook("-5", 0, 990), null);
  assert.equal(stakeOutlook("abc", 0, 990), null);
});

test("shares and waits read as people say them", () => {
  assert.equal(shareText(outlook(1, 2_400)!), `1 in 2${G}400 (0.04%)`);
  assert.equal(shareText(outlook(1, 4)!), "1 in 4 (25%)");
  assert.equal(shareText(outlook(3, 200)!), "1 in 67 (1.5%)");
  assert.equal(shareText(outlook(1, 1_000_000)!), `1 in 1${G}000${G}000 (under 0.01%)`);
  assert.equal(waitText(60), "about 1 minute");
  assert.equal(waitText(150), "about 2.5 minutes");
  assert.equal(waitText(2 * 3_600), "about 2 hours");
  assert.equal(waitText(40 * 86_400), "about 40 days");
  assert.equal(waitText(1.5 * 365.25 * 86_400), "about 1.5 years");
  assert.equal(waitText(2_400 * BLOCK_SECONDS), "about 1.7 days");
  assert.equal(LONG_WAIT_SECONDS, 30 * 86_400);
});
