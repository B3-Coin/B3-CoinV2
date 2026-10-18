// Run with: node --experimental-strip-types --test web/test/
import assert from "node:assert/strict";
import { test } from "node:test";

import { decimal, formatB3, formatPrice, formatQuantity, neededPlaces, parsePrice, parseQuantity, pricePlaces, quantityPlaces, spread } from "../src/tradefmt.ts";

const NBSP = " ";

test("decimal writes values exactly and never cuts digits", () => {
  assert.equal(decimal(435000000n, 9, 2), "0.435");
  assert.equal(decimal(400000000n, 9, 2), "0.40");
  assert.equal(decimal(5n, 2, 0), "0.05");
  assert.equal(decimal(500n, 2, 0), "5");
  assert.equal(decimal(123456789123456789n, 9, 0), "123456789.123456789");
  assert.equal(decimal(-15n, 1, 3), "-1.500");
});

test("needed places ignore trailing zeros", () => {
  assert.equal(neededPlaces(0n, 9), 0);
  assert.equal(neededPlaces(100n, 9), 7);
  assert.equal(neededPlaces(1_000_000_000n, 9), 0);
});

test("prices are B3 per token at the tick's places", () => {
  // The fixture market: 2 decimals, tick 1 atom per base unit.
  const units = { decimals: 2, tick: 1n, lot: 1n };
  assert.equal(pricePlaces(units), 7);
  // 30 000 000 atoms per 0.01 token is 3 B3 per token.
  assert.equal(formatPrice(30_000_000n, units), "3.0000000");
  assert.equal(formatPrice(30_000_001n, units), "3.0000001");
  // A six-decimal token: one atom per base unit is 0.001 B3 per token.
  const tusd = { decimals: 6, tick: 1n, lot: 1n };
  assert.equal(pricePlaces(tusd), 3);
  assert.equal(formatPrice(1_000n, tusd), "1.000");
  assert.equal(formatPrice(997n, tusd), "0.997");
  // A coarser tick needs fewer places: 10 atoms is 0.01 B3 per token.
  assert.equal(pricePlaces({ ...tusd, tick: 10n }), 2);
  // More decimals than B3: the price is a whole multiple.
  const fine = { decimals: 12, tick: 1n, lot: 1n };
  assert.equal(formatPrice(2_500n, fine), `2${NBSP}500${NBSP}000`);
});

test("quantities share the places their column needs", () => {
  assert.equal(quantityPlaces([300n, 5n, 120n], 2), 2);
  assert.equal(quantityPlaces([300n, 500n], 2), 0);
  assert.equal(formatQuantity(5n, 2, 2), "0.05");
  assert.equal(formatQuantity(1_234_500n, 2, 2), `12${NBSP}345.00`);
});

test("B3 amounts keep at least two decimals", () => {
  assert.equal(formatB3(90_000_000n), "0.09");
  assert.equal(formatB3(1_000_000_000n), "1.00");
  assert.equal(formatB3(1n), "0.000000001");
});

test("the spread is exact and absent for a crossed book", () => {
  const units = { decimals: 2, tick: 1n, lot: 1n };
  const s = spread(30_000_000n, 80_000_000n, units);
  assert.deepEqual(s, { difference: "5.0000000", percent: "90.91" });
  assert.equal(spread(80n, 80n, units), null);
  assert.equal(spread(81n, 80n, units), null);
  // Rounds half up: 200 x 1 / 400 = 0.5 %.
  assert.equal(spread(199n, 201n, units)?.percent, "1.00");
  assert.equal(spread(1999n, 2001n, units)?.percent, "0.10");
});

test("typed prices and quantities convert exactly or are refused", () => {
  const units = { decimals: 2, tick: 1n, lot: 1n };
  assert.deepEqual(parsePrice("3", units), { value: 30_000_000n });
  assert.deepEqual(parsePrice("3.0000001", units), { value: 30_000_001n });
  assert.deepEqual(parsePrice(".5", units), { value: 5_000_000n });
  assert.deepEqual(parsePrice(`1${NBSP}000`, units), { value: 10_000_000_000n });
  // Finer than one atom per base unit: refused, not rounded.
  assert.ok("error" in parsePrice("3.00000001", units));
  for (const bad of ["", "abc", "-1", "1e3", "1,5", "0", "0.0"]) assert.ok("error" in parsePrice(bad, units), bad);
  assert.deepEqual(parseQuantity("0.03", units), { value: 3n });
  assert.deepEqual(parseQuantity("2", units), { value: 200n });
  assert.ok("error" in parseQuantity("0.001", units));
  // Off the market's steps.
  const coarse = { decimals: 6, tick: 10n, lot: 1000n };
  assert.deepEqual(parsePrice("0.02", coarse), { value: 20n });
  assert.ok("error" in parsePrice("0.021", coarse));
  assert.deepEqual(parseQuantity("0.002", coarse), { value: 2000n });
  assert.ok("error" in parseQuantity("0.0021", coarse));
  // More decimals than B3: a price must be a whole number of atoms per base unit.
  const fine = { decimals: 12, tick: 1n, lot: 1n };
  assert.deepEqual(parsePrice("2000", fine), { value: 2n });
  assert.ok("error" in parsePrice("1500", fine));
});
