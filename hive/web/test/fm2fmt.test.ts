// Run with: node --experimental-strip-types --test web/test/
import assert from "node:assert/strict";
import { test } from "node:test";

import {
  amount,
  amountText,
  b3,
  baseAtoms,
  depth,
  feeRate,
  lotText,
  maxLots,
  outcomeText,
  parseAmount,
  parsePrice,
  parseWhole,
  percent,
  price,
  quoteAtoms,
  sellerFee,
  shortId,
  spread,
  tickText,
  whole,
  type Asset,
  type Market,
} from "../src/fm2fmt.ts";

const G = " ";
const B3: Asset = { name: "B3", id: "00".repeat(32), native: true };
const RUSD: Asset = { name: "rUSD", id: "11".repeat(32), native: false };
const TOK: Asset = { name: "TOK", id: "22".repeat(32), native: false };
// The two markets of the regtest genesis document fm2-keygen writes.
const B3_RUSD: Market = { name: "B3/rUSD", id: "aa".repeat(32), base: B3, quote: RUSD, base_atoms_per_lot: "1000000", quote_atoms_per_tick: "1" };
const TOK_RUSD: Market = { name: "TOK/rUSD", id: "bb".repeat(32), base: TOK, quote: RUSD, base_atoms_per_lot: "1", quote_atoms_per_tick: "10" };

test("native B3 is shown in B3, every other asset in atoms", () => {
  assert.equal(b3(99_990_000_000n), "99.99");
  assert.equal(b3(1_234_000_000_001n), `1${G}234.000000001`);
  assert.equal(b3(0n), "0");
  assert.equal(b3(5n, 2), "0.000000005");
  assert.equal(b3(2_000_000_000n, 2), "2.00");
  assert.equal(b3(-1_500_000_000n), "−1.5");
  assert.equal(amount(1_500_000_000n, B3), "1.5");
  assert.equal(amount(1_234_567n, RUSD), `1${G}234${G}567`);
  assert.equal(amountText(400n, RUSD), "400 rUSD atoms");
  assert.equal(amountText(4_000_000n, B3), "0.004 B3");
  assert.equal(whole(-1000n), `−1${G}000`);
});

test("typed numbers are exact or refused", () => {
  assert.equal(parseWhole("1 000"), 1000n);
  assert.equal(parseWhole(`1${G}000`), 1000n);
  assert.equal(parseWhole("0"), 0n);
  for (const bad of ["", "1.5", "-1", "1e3", "abc", "0x10", "12345678901234567890"]) assert.equal(parseWhole(bad), null, bad);
  assert.equal(parseAmount("1.5", B3), 1_500_000_000n);
  assert.equal(parseAmount("0.000000001", B3), 1n);
  assert.equal(parseAmount("3", B3), 3_000_000_000n);
  assert.equal(parseAmount("3.", B3), 3_000_000_000n);
  // A tenth decimal would be less than one atom.
  assert.equal(parseAmount("0.0000000001", B3), null);
  assert.equal(parseAmount(".5", B3), null);
  assert.equal(parseAmount("1.5", RUSD), null);
  assert.equal(parseAmount("15", RUSD), 15n);
});

test("a price is for one lot, in whole ticks", () => {
  assert.equal(price(100n, B3_RUSD), "100");
  assert.equal(price(7n, TOK_RUSD), "70");
  assert.equal(parsePrice("100", B3_RUSD), 100n);
  assert.equal(parsePrice("70", TOK_RUSD), 7n);
  // 75 atoms is between two ticks of 10.
  assert.equal(parsePrice("75", TOK_RUSD), null);
  assert.equal(parsePrice("0", B3_RUSD), null);
  assert.equal(parsePrice("", B3_RUSD), null);
  assert.equal(lotText(B3_RUSD), "0.001 B3");
  assert.equal(lotText(TOK_RUSD), "1 TOK atoms");
  assert.equal(tickText(TOK_RUSD), "10 rUSD atoms");
});

test("an order's amounts follow from lots, ticks and the market", () => {
  assert.equal(baseAtoms(4n, B3_RUSD), 4_000_000n);
  // The fill of the captured fixture: 4 lots at 100 ticks is 400 atoms.
  assert.equal(quoteAtoms(4n, 100n, B3_RUSD), 400n);
  assert.equal(quoteAtoms(3n, 7n, TOK_RUSD), 210n);
  assert.equal(maxLots("buy", 1000n, 100n, B3_RUSD), 10n);
  assert.equal(maxLots("buy", 999n, 100n, B3_RUSD), 9n);
  assert.equal(maxLots("buy", 1000n, 0n, B3_RUSD), 0n);
  assert.equal(maxLots("sell", 99_990_000_000n, 100n, B3_RUSD), 99_990n);
  assert.equal(maxLots("sell", 0n, 100n, B3_RUSD), 0n);
});

test("the seller's fee rounds up over the order", () => {
  // The fixture's seller paid 1 atom on 400 at 100 ppm.
  assert.equal(sellerFee(400n, 100), 1n);
  assert.equal(sellerFee(10_000n, 100), 1n);
  assert.equal(sellerFee(10_001n, 100), 2n);
  assert.equal(sellerFee(0n, 100), 0n);
  assert.equal(feeRate(100), "0.01%");
  assert.equal(feeRate(2500), "0.25%");
  assert.equal(feeRate(1_000_000), "100%");
});

test("a ladder gets running totals and a spread", () => {
  const asks = [
    { price_ticks: "100", lots: "6", orders: 1 },
    { price_ticks: "101", lots: "40", orders: 2 },
  ];
  const bids = [{ price_ticks: "98", lots: "25", orders: 1 }];
  assert.deepEqual(
    depth(asks).map((d) => [d.ticks, d.lots, d.total]),
    [
      [100n, 6n, 6n],
      [101n, 40n, 46n],
    ],
  );
  assert.deepEqual(spread(bids, asks), { ticks: 2n, bps: 200n });
  assert.equal(percent(200n), "2.00%");
  assert.equal(percent(5n), "0.05%");
  assert.equal(spread([], asks), null);
  // A crossed book has no spread to show.
  assert.equal(spread([{ price_ticks: "100", lots: "1", orders: 1 }], asks), null);
});

test("outcome codes read as plain words, unknown ones as they are", () => {
  assert.equal(outcomeText("FILLED"), "Filled");
  assert.match(outcomeText("REJ_WOULD_CROSS"), /post-only/);
  assert.equal(outcomeText("code 200"), "code 200");
  assert.equal(shortId("ab".repeat(32)), "abababab…ababab");
  assert.equal(shortId("abc"), "abc");
});
