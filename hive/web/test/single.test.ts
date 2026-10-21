// Run with: node --experimental-strip-types --test web/test/
import assert from "node:assert/strict";
import { test } from "node:test";

import { single } from "../src/single.ts";

/** A refresh whose replies arrive when the test releases them. */
function gated() {
  const shown: number[] = [];
  const release: (() => void)[] = [];
  let calls = 0;
  const refresh = single(async (current) => {
    const call = ++calls;
    await new Promise<void>((resolve) => release.push(resolve));
    if (current()) shown.push(call);
  });
  return { refresh, shown, release, calls: () => calls };
}

const tick = () => new Promise((resolve) => setTimeout(resolve, 0));

test("a refresh on its own shows its reply", async () => {
  const g = gated();
  const done = g.refresh();
  await tick();
  g.release.shift()?.();
  await done;
  assert.deepEqual(g.shown, [1]);
});

test("a request while one runs is not lost, and the old reply is not shown", async () => {
  const g = gated();
  const first = g.refresh();
  await tick();
  // For example a wallet switch while the old wallet is being read.
  void g.refresh();
  void g.refresh();
  g.release.shift()?.();
  await tick();
  assert.equal(g.calls(), 2, "runs once more for any number of requests");
  g.release.shift()?.();
  await first;
  assert.deepEqual(g.shown, [2]);
});

test("a failing refresh does not block the next one", async () => {
  let n = 0;
  const refresh = single(async () => {
    n++;
    if (n === 1) throw new Error("node busy");
  });
  await assert.rejects(refresh());
  await refresh();
  assert.equal(n, 2);
});
