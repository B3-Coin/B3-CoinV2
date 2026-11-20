// Run with: node --experimental-strip-types --test web/test/
import assert from "node:assert/strict";
import { test } from "node:test";

import {
  amount,
  amountText,
  baseAtoms,
  decimals,
  depth,
  feeRate,
  lotText,
  lotsIn,
  maxLots,
  nearestPrice,
  outcomeText,
  parseAmount,
  parsePrice,
  parseWhole,
  percent,
  plain,
  plainPrice,
  plan,
  price,
  priceFor,
  priceText,
  priceUnit,
  quoteAtoms,
  reach,
  refusal,
  sellerFee,
  shortId,
  spread,
  tickText,
  tifText,
  whole,
  type Asset,
  type Level,
  type Market,
} from "../src/fm2fmt.ts";

const G = " "; // the narrow no-break space digits are grouped with
const B3: Asset = { name: "B3", id: "00".repeat(32), native: true };
const RUSD: Asset = { name: "rUSD", id: "11".repeat(32), native: false };
const TOK: Asset = { name: "TOK", id: "22".repeat(32), native: false };
// The two markets of the regtest genesis document fm2-keygen writes.
const B3_RUSD: Market = { name: "B3/rUSD", id: "aa".repeat(32), base: B3, quote: RUSD, base_atoms_per_lot: "1000000", quote_atoms_per_tick: "1" };
const TOK_RUSD: Market = { name: "TOK/rUSD", id: "bb".repeat(32), base: TOK, quote: RUSD, base_atoms_per_lot: "1", quote_atoms_per_tick: "10" };
// Markets a genesis document could also define.
const TOK_B3: Market = { name: "TOK/B3", id: "cc".repeat(32), base: TOK, quote: B3, base_atoms_per_lot: "100", quote_atoms_per_tick: "5000" };
const ODD: Market = { name: "B3/rUSD", id: "dd".repeat(32), base: B3, quote: RUSD, base_atoms_per_lot: "300000", quote_atoms_per_tick: "1" };

test("native B3 has nine decimals, every other asset none", () => {
  assert.equal(decimals(B3), 9);
  assert.equal(decimals(RUSD), 0);
  assert.equal(amount(99_990_000_000n, B3), "99.99");
  assert.equal(amount(1_234_000_000_001n, B3), `1${G}234.000000001`);
  assert.equal(amount(0n, B3), "0");
  assert.equal(amount(2_000_000_000n, B3, 2), "2.00");
  assert.equal(amount(5n, B3, 2), "0.000000005");
  assert.equal(amount(-1_500_000_000n, B3), "−1.5");
  assert.equal(amount(1_234_567n, RUSD), `1${G}234${G}567`);
  // An asset without decimals never grows any.
  assert.equal(amount(7n, RUSD, 2), "7");
  assert.equal(amountText(400n, RUSD), "400 rUSD");
  assert.equal(amountText(4_000_000n, B3), "0.004 B3");
  assert.equal(plain(1_234_500_000_000n, B3), "1234.5");
  assert.equal(plain(1_234_567n, RUSD), "1234567");
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
  assert.equal(parseAmount("1 000.25", B3), 1_000_250_000_000n);
  // More decimals than the asset has, a comma, a sign: nothing is guessed.
  for (const bad of ["", "0.0000000001", "1,5", "-1", ".5", "1.5.2", "1e3"]) assert.equal(parseAmount(bad, B3), null, bad);
  assert.equal(parseAmount("250", RUSD), 250n);
  assert.equal(parseAmount("2.5", RUSD), null);
  // What an input shows parses back to the same atoms.
  for (const atoms of [0n, 1n, 999_999_999n, 1_000_000_000n, 123_456_789_012_345n]) assert.equal(parseAmount(plain(atoms, B3), B3), atoms);
});

test("a price is for one whole unit of the base when that is exact", () => {
  // One lot is 0.001 B3 and one tick is 1 rUSD for it: 1 000 rUSD per B3.
  assert.equal(priceFor(B3_RUSD), "B3");
  assert.equal(priceUnit(B3_RUSD), "rUSD per B3");
  assert.equal(price(101n, B3_RUSD), `101${G}000`);
  assert.equal(priceText(101n, B3_RUSD), `101${G}000 rUSD per B3`);
  assert.equal(tickText(B3_RUSD), `1${G}000 rUSD`);
  assert.equal(lotText(B3_RUSD), "0.001 B3");
  // One lot is one TOK and one tick is 10 rUSD for it.
  assert.equal(priceUnit(TOK_RUSD), "rUSD per TOK");
  assert.equal(price(7n, TOK_RUSD), "70");
  assert.equal(tickText(TOK_RUSD), "10 rUSD");
  assert.equal(lotText(TOK_RUSD), "1 TOK");
  // 100 TOK for 5 000 atoms of B3 a tick: 50 atoms of B3 per TOK.
  assert.equal(priceUnit(TOK_B3), "B3 per TOK");
  assert.equal(price(3n, TOK_B3), "0.00000015");
  assert.equal(tickText(TOK_B3), "0.00000005 B3");
});

test("a price is for one lot when a whole unit has no exact price", () => {
  // A lot of 0.0003 B3 does not go into one B3 a whole number of times.
  assert.equal(priceFor(ODD), "0.0003 B3");
  assert.equal(priceUnit(ODD), "rUSD per 0.0003 B3");
  assert.equal(price(41n, ODD), "41");
  assert.equal(tickText(ODD), "1 rUSD");
});

test("a typed price is a whole number of ticks or refused, and can be rounded", () => {
  assert.equal(parsePrice("101000", B3_RUSD), 101n);
  assert.equal(parsePrice("101 000", B3_RUSD), 101n);
  assert.equal(parsePrice("101500", B3_RUSD), null);
  assert.equal(parsePrice("0", B3_RUSD), null);
  assert.equal(parsePrice("70", TOK_RUSD), 7n);
  assert.equal(parsePrice("75", TOK_RUSD), null);
  assert.equal(parsePrice("0.00000015", TOK_B3), 3n);
  assert.equal(nearestPrice("101499", B3_RUSD), 101n);
  assert.equal(nearestPrice("101500", B3_RUSD), 102n);
  assert.equal(nearestPrice("3", B3_RUSD), 1n);
  assert.equal(nearestPrice("abc", B3_RUSD), null);
  // Every price the page shows parses back to its ticks.
  for (const m of [B3_RUSD, TOK_RUSD, TOK_B3, ODD]) {
    for (const ticks of [1n, 2n, 99n, 101n, 123_456n]) {
      assert.equal(parsePrice(plainPrice(ticks, m), m), ticks, `${m.name} ${ticks}`);
      assert.equal(parsePrice(price(ticks, m), m), ticks, `${m.name} ${ticks} grouped`);
    }
  }
});

test("lots and ticks turn into atoms exactly", () => {
  assert.equal(baseAtoms(5n, B3_RUSD), 5_000_000n);
  assert.equal(quoteAtoms(5n, 101n, B3_RUSD), 505n);
  assert.equal(quoteAtoms(3n, 7n, TOK_RUSD), 210n);
  assert.equal(lotsIn(5_999_999n, B3_RUSD), 5n);
  assert.equal(lotsIn(999_999n, B3_RUSD), 0n);
  assert.equal(lotsIn(-1n, B3_RUSD), 0n);
  assert.equal(maxLots("buy", 1000n, 101n, B3_RUSD), 9n);
  assert.equal(maxLots("buy", 1000n, 0n, B3_RUSD), 0n);
  assert.equal(maxLots("sell", 5_500_000n, 101n, B3_RUSD), 5n);
  assert.equal(maxLots("sell", 0n, 101n, B3_RUSD), 0n);
});

test("the seller's fee rounds up, as the engine charges it", () => {
  assert.equal(sellerFee(0n, 100), 0n);
  assert.equal(sellerFee(1n, 100), 1n);
  assert.equal(sellerFee(10_000n, 100), 1n);
  assert.equal(sellerFee(10_001n, 100), 2n);
  assert.equal(sellerFee(1_000_000n, 2500), 2500n);
  assert.equal(feeRate(100), "0.01%");
  assert.equal(feeRate(2500), "0.25%");
  assert.equal(feeRate(1_000_000), "100%");
  assert.equal(feeRate(0), "0%");
});

const level = (price_ticks: number, lots: number, orders = 1): Level => ({ price_ticks: String(price_ticks), lots: String(lots), orders });

test("a ladder's depth and spread", () => {
  const asks = [level(101, 34, 2), level(103, 5)];
  const bids = [level(98, 25), level(97, 4)];
  assert.deepEqual(depth(asks), [
    { ticks: 101n, lots: 34n, total: 34n, orders: 2 },
    { ticks: 103n, lots: 5n, total: 39n, orders: 1 },
  ]);
  assert.deepEqual(spread(bids, asks), { ticks: 3n, bps: 297n });
  assert.equal(percent(297n), "2.97%");
  assert.equal(percent(5n), "0.05%");
  assert.equal(spread([], asks), null);
  assert.equal(spread([level(101, 1)], asks), null);
});

// ---- trading at once -------------------------------------------------------

const ASKS = [level(101, 34, 2), level(103, 5), level(110, 100)];
const BIDS = [level(98, 25), level(97, 4), level(90, 10)];
const RICH = 1_000_000_000n;

test("buying an amount of the base sets aside its lots at the last price reached", () => {
  // 0.005 B3 is five lots, all at the best price.
  const one = plan("buy", { of: "base", atoms: 5_000_000n }, ASKS, RICH, B3_RUSD, 100);
  assert.deepEqual(one, { lots: 5n, limit: 101n, best: 101n, gross: 505n, ceiling: 505n, fee: 0n, short: null, offered: 139n });
  assert.equal(reach(one), 0n);
  // 0.036 B3 reaches the second level: 34 at 101 and 2 at 103, but all 36
  // are set aside at 103.
  const two = plan("buy", { of: "base", atoms: 36_000_000n }, ASKS, RICH, B3_RUSD, 100);
  assert.equal(two.lots, 36n);
  assert.equal(two.limit, 103n);
  assert.equal(two.gross, 34n * 101n + 2n * 103n);
  assert.equal(two.ceiling, 36n * 103n);
  assert.equal(reach(two), 198n); // 2 in 101
  // What is left of a lot is not traded.
  assert.equal(plan("buy", { of: "base", atoms: 5_999_999n }, ASKS, RICH, B3_RUSD, 100).lots, 5n);
});

test("buying with a budget never sets aside more than the budget", () => {
  // 505 rUSD pays for five lots at 101.
  assert.equal(plan("buy", { of: "quote", atoms: 505n }, ASKS, RICH, B3_RUSD, 100).lots, 5n);
  // 504 pays for four; nothing is rounded up.
  assert.equal(plan("buy", { of: "quote", atoms: 504n }, ASKS, RICH, B3_RUSD, 100).lots, 4n);
  // 3 500 pays for all 34 at 101 (3 434). One more lot would set all 35
  // aside at 103, which is 3 605.
  const stay = plan("buy", { of: "quote", atoms: 3500n }, ASKS, RICH, B3_RUSD, 100);
  assert.equal(stay.lots, 34n);
  assert.equal(stay.limit, 101n);
  assert.equal(stay.ceiling, 3434n);
  assert.equal(stay.short, null);
  // 3 605 is exactly enough for the 35th.
  const next = plan("buy", { of: "quote", atoms: 3605n }, ASKS, RICH, B3_RUSD, 100);
  assert.equal(next.lots, 35n);
  assert.equal(next.ceiling, 3605n);
  assert.equal(next.gross, 34n * 101n + 103n);
  // Whatever the budget, the ceiling stays within it.
  for (let budget = 0n; budget < 20_000n; budget += 37n) {
    const p = plan("buy", { of: "quote", atoms: budget }, ASKS, RICH, B3_RUSD, 100);
    assert.ok(p.ceiling <= budget, `budget ${budget}`);
    assert.ok(p.gross <= p.ceiling, `budget ${budget}`);
    // And one more lot would not fit.
    if (p.lots < p.offered) {
      const more = plan("buy", { of: "base", atoms: baseAtoms(p.lots + 1n, B3_RUSD) }, ASKS, RICH, B3_RUSD, 100);
      assert.ok(more.ceiling > budget, `budget ${budget} leaves room`);
    }
  }
});

test("a buy says why it is for less than was asked", () => {
  // Less than one step.
  assert.deepEqual(plan("buy", { of: "base", atoms: 999_999n }, ASKS, RICH, B3_RUSD, 100).short, "small");
  assert.deepEqual(plan("buy", { of: "quote", atoms: 100n }, ASKS, RICH, B3_RUSD, 100).short, "small");
  // More than the book offers.
  const all = plan("buy", { of: "base", atoms: 500_000_000n }, ASKS, RICH, B3_RUSD, 100);
  assert.equal(all.lots, 139n);
  assert.equal(all.short, "book");
  assert.equal(all.limit, 110n);
  const flush = plan("buy", { of: "quote", atoms: 10_000_000n }, ASKS, RICH, B3_RUSD, 100);
  assert.equal(flush.lots, 139n);
  assert.equal(flush.short, "book");
  // A budget that buys every lot on offer and no more is not short.
  assert.equal(plan("buy", { of: "quote", atoms: 139n * 110n }, ASKS, RICH, B3_RUSD, 100).short, null);
  // More than the balance pays for: 1 000 rUSD sets aside nine lots at 101.
  const poor = plan("buy", { of: "base", atoms: 20_000_000n }, ASKS, 1000n, B3_RUSD, 100);
  assert.equal(poor.lots, 9n);
  assert.equal(poor.short, "funds");
  assert.ok(poor.ceiling <= 1000n);
  const over = plan("buy", { of: "quote", atoms: 5000n }, ASKS, 1000n, B3_RUSD, 100);
  assert.equal(over.lots, 9n);
  assert.equal(over.short, "funds");
  // The balance counts at the price reached, not at the prices paid: 3 500
  // cannot set 35 lots aside at 103, although they would cost 3 537.
  const edge = plan("buy", { of: "base", atoms: 35_000_000n }, ASKS, 3550n, B3_RUSD, 100);
  assert.equal(edge.lots, 34n);
  assert.equal(edge.short, "funds");
  // Nothing on offer.
  const none = plan("buy", { of: "base", atoms: 5_000_000n }, [], RICH, B3_RUSD, 100);
  assert.deepEqual(none, { lots: 0n, limit: 0n, best: 0n, gross: 0n, ceiling: 0n, fee: 0n, short: "book", offered: 0n });
  assert.equal(plan("buy", { of: "quote", atoms: 5000n }, [], 10n, B3_RUSD, 100).short, "book");
  assert.equal(reach(none), 0n);
});

test("selling walks the bids down and takes the fee from what they bring", () => {
  // 0.027 B3: 25 lots at 98 and 2 at 97.
  const p = plan("sell", { of: "base", atoms: 27_000_000n }, BIDS, 50_000_000n, B3_RUSD, 100);
  assert.equal(p.lots, 27n);
  assert.equal(p.limit, 97n);
  assert.equal(p.best, 98n);
  assert.equal(p.gross, 25n * 98n + 2n * 97n);
  assert.equal(p.fee, 1n); // 0.01% of 2 644, rounded up
  assert.equal(p.ceiling, 0n);
  assert.equal(p.short, null);
  assert.equal(reach(p), 102n);
  // No more than the account holds, in whole lots.
  const held = plan("sell", { of: "base", atoms: 27_000_000n }, BIDS, 3_999_999n, B3_RUSD, 100);
  assert.equal(held.lots, 3n);
  assert.equal(held.short, "funds");
  // No more than is bid for.
  const deep = plan("sell", { of: "base", atoms: 100_000_000n }, BIDS, RICH, B3_RUSD, 100);
  assert.equal(deep.lots, 39n);
  assert.equal(deep.short, "book");
  assert.equal(deep.limit, 90n);
  assert.equal(plan("sell", { of: "base", atoms: 1n }, BIDS, RICH, B3_RUSD, 100).short, "small");
  assert.equal(plan("sell", { of: "base", atoms: 5_000_000n }, [], RICH, B3_RUSD, 100).short, "book");
});

test("selling for an amount takes the fewest lots that bring it after the fee", () => {
  // One lot at 98 brings 98 less a fee of 1.
  for (const [want, lots] of [
    [1n, 1n],
    [97n, 1n],
    [98n, 2n],
    [2449n, 25n], // 25 x 98 = 2 450, fee 1
    [2450n, 26n],
  ] as const) {
    const p = plan("sell", { of: "quote", atoms: want }, BIDS, RICH, B3_RUSD, 100);
    assert.equal(p.lots, lots, `want ${want}`);
    assert.ok(p.gross - p.fee >= want, `want ${want} is reached`);
    assert.equal(p.short, null);
    // One lot fewer would not bring it.
    if (lots > 1n) {
      const less = plan("sell", { of: "base", atoms: baseAtoms(lots - 1n, B3_RUSD) }, BIDS, RICH, B3_RUSD, 100);
      assert.ok(less.gross - less.fee < want, `want ${want} needs every lot`);
    }
  }
  // More than every bid brings.
  const far = plan("sell", { of: "quote", atoms: 1_000_000n }, BIDS, RICH, B3_RUSD, 100);
  assert.equal(far.lots, 39n);
  assert.equal(far.short, "book");
  // More than the account can sell.
  const thin = plan("sell", { of: "quote", atoms: 2000n }, BIDS, 5_000_000n, B3_RUSD, 100);
  assert.equal(thin.lots, 5n);
  assert.equal(thin.short, "funds");
});

test("codes and ids read as words", () => {
  assert.equal(outcomeText("FILLED"), "Filled");
  assert.equal(outcomeText("REJ_WOULD_CROSS"), "Refused: a post-only order may not trade at once");
  assert.equal(outcomeText("SOMETHING_NEW"), "SOMETHING_NEW");
  assert.equal(refusal("REJ_NO_RECIPIENT"), "the account it was sent to does not exist on this network yet");
  assert.equal(refusal("REJ_NEW"), "the network answered REJ_NEW");
  assert.equal(tifText("gtc"), "Waits until it is filled or cancelled");
  assert.equal(tifText("fok"), "fok");
  assert.equal(shortId("ab".repeat(32)), "abababab…ababab");
  assert.equal(shortId("abcdef"), "abcdef");
});
