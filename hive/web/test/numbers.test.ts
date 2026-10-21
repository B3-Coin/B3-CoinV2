// Run with: node --experimental-strip-types --test web/test/
import assert from "node:assert/strict";
import { test } from "node:test";

import { count, measure } from "../shared/ui.ts";

const G = " ";

test("counts group as amounts do, whatever the locale", () => {
  assert.equal(count(0), "0");
  assert.equal(count(999), "999");
  assert.equal(count(1234567), `1${G}234${G}567`);
  assert.equal(count(-1500), `−1${G}500`);
});

test("measures keep a point and at most the places asked for", () => {
  assert.equal(measure(12.345, 1), "12.3");
  assert.equal(measure(1234.5, 1), `1${G}234.5`);
  assert.equal(measure(99.999, 2), "100");
  assert.equal(measure(7, 0), "7");
});
