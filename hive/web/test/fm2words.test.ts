// Run with: node --experimental-strip-types --test web/test/
import assert from "node:assert/strict";
import { test } from "node:test";

import { plan, type Asset, type Level, type Market } from "../src/fm2fmt.ts";
import { advise, numberProblem, ordered, size, tell, type Asking, type Fill, type Intent, type Place, type Row } from "../src/fm2words.ts";

const G = " "; // the narrow no-break space digits are grouped with
const B3: Asset = { name: "B3", id: "00".repeat(32), native: true };
const RUSD: Asset = { name: "rUSD", id: "11".repeat(32), native: false };
// The B3 market of the regtest genesis document fm2-keygen writes: one lot
// is 0.001 B3 and one tick is 1 rUSD a lot, so 101 ticks read as 101 000
// rUSD per B3.
const M: Market = { name: "B3/rUSD", id: "aa".repeat(32), base: B3, quote: RUSD, base_atoms_per_lot: "1000000", quote_atoms_per_tick: "1" };

const level = (price_ticks: number, lots: number): Level => ({ price_ticks: String(price_ticks), lots: String(lots), orders: 1 });
const ASKS = [level(101, 34), level(103, 5), level(110, 100)];
const BIDS = [level(98, 25), level(97, 4), level(90, 10)];
const RICH = 1_000_000_000n;

// ---- rows of the history ---------------------------------------------------

const place = (side: "buy" | "sell", tif: string, ticks: number, lots: number): Place => ({
  action: "place",
  market: M,
  side,
  tif,
  price_ticks: String(ticks),
  lots: String(lots),
  base: { asset: B3, atoms: String(lots * 1_000_000) },
  quote: { asset: RUSD, atoms: String(lots * ticks) },
});
const transfer: Intent = { action: "transfer", to: "ab".repeat(32), amount: { asset: RUSD, atoms: "100" } };
const signed = { label: "a1", created: 0, action_id: "cd".repeat(32), nonce: "7", ref_slot: "10", window_slots: 64 };
const open = (intent: Intent): Row => ({ ...signed, intent, state: "open", note: "" });
const expired = (intent: Intent): Row => ({ ...signed, intent, state: "expired", height: "80" });
const included = (intent: Intent, code_name: string, accepted: boolean, fills: Fill[] = []): Row => ({
  ...signed,
  intent,
  state: "included",
  height: "12",
  index: 0,
  code: 0,
  code_name,
  accepted,
  remaining_lots: "0",
  fills,
  deltas: [],
});
const fill = (side: "buy" | "sell", ticks: number, lots: number, fee: string | null = null): Fill => ({
  market: M.id,
  side,
  price_ticks: String(ticks),
  lots: String(lots),
  quote: { asset: RUSD, atoms: String(ticks * lots) },
  fee_atoms: fee,
});

test("amounts and orders in a few words", () => {
  assert.equal(size(5n, M), "0.005 B3");
  assert.equal(size(1500n, M), "1.5 B3");
  assert.equal(ordered(place("buy", "gtc", 99, 5)), `Buy 0.005 B3 at 99${G}000 rUSD per B3`);
  assert.equal(ordered(place("sell", "post_only", 105, 20)), `Sell 0.02 B3 at 105${G}000 rUSD per B3`);
});

test("a typed amount that is not one says how to fix it", () => {
  assert.equal(numberProblem("0,5", B3), "Use a dot for decimals, like 0.5.");
  assert.equal(numberProblem("0.1234567891", B3), "B3 has at most 9 decimals.");
  assert.equal(numberProblem("1.5", RUSD), "rUSD has no decimals on this network. Enter a whole number.");
  assert.equal(numberProblem("abc", RUSD), "Enter a number, like 0.5 or 100.");
  assert.equal(numberProblem("-3", B3), "Enter a number, like 0.5 or 100.");
});

test("what was signed and is not confirmed yet reads as going on", () => {
  const waiting = { meta: "Waiting for the network to confirm it", detail: "", tone: "wait", icon: "clock", result: "warning" };
  assert.deepEqual(tell(open(place("buy", "ioc", 101, 5))), { title: "Buying 0.005 B3", ...waiting });
  assert.deepEqual(tell(open(place("sell", "ioc", 98, 5))), { title: "Selling 0.005 B3", ...waiting });
  assert.deepEqual(tell(open(place("sell", "gtc", 105, 5))), { title: "Placing an order to sell 0.005 B3", ...waiting });
  assert.equal(tell(open({ action: "cancel", order: "ef".repeat(32), placed: null })).title, "Cancelling an order");
  assert.equal(tell(open({ action: "cancel_all" })).title, "Cancelling all orders");
  assert.equal(tell(open(transfer)).title, "Sending 100 rUSD");
  assert.equal(tell(open({ action: "other", kind: 9 })).title, "Something you signed");
});

test("what the network can no longer take says that nothing changed", () => {
  const gone = "The network did not take it in time, and now it never can. Nothing changed in your account.";
  const told = tell(expired(place("buy", "ioc", 101, 5)));
  assert.equal(told.title, "0.005 B3 was not bought");
  assert.equal(told.detail, gone);
  assert.equal(told.result, "warning");
  assert.equal(tell(expired(place("sell", "ioc", 98, 5))).title, "0.005 B3 was not sold");
  assert.equal(tell(expired(place("buy", "gtc", 99, 5))).title, "The order was not placed");
  assert.equal(tell(expired({ action: "cancel", order: "ef".repeat(32), placed: null })).title, "The order was not cancelled");
  assert.equal(tell(expired({ action: "cancel_all" })).title, "The orders were not cancelled");
  assert.equal(tell(expired(transfer)).title, "100 rUSD was not sent");
  assert.equal(tell(expired({ action: "other", kind: 9 })).title, "It did not happen");
});

test("a buy that traded says what was bought and for how much", () => {
  // All of it, at one price.
  assert.deepEqual(tell(included(place("buy", "ioc", 101, 9), "FILLED", true, [fill("buy", 101, 9)])), {
    title: "Bought 0.009 B3 for 909 rUSD",
    meta: `at 101${G}000 rUSD per B3`,
    lead: `At 101${G}000 rUSD per B3.`,
    detail: "",
    tone: "buy",
    icon: "arrow-down",
    result: "success",
  });
  // All of it, across two prices: what was paid is the sum of the fills,
  // not the limit times the size.
  const two = tell(included(place("buy", "ioc", 103, 36), "FILLED", true, [fill("buy", 101, 34), fill("buy", 103, 2)]));
  assert.equal(two.title, `Bought 0.036 B3 for 3${G}640 rUSD`);
  assert.equal(two.meta, `at 101${G}000 to 103${G}000 rUSD per B3`);
  assert.equal(two.lead, `At 101${G}000 to 103${G}000 rUSD per B3.`);
  assert.equal(two.result, "success");
});

test("a sale says what it brought after the seller's fee", () => {
  const told = tell(included(place("sell", "ioc", 97, 27), "FILLED", true, [fill("sell", 98, 25, "1"), fill("sell", 97, 2, "1")]));
  assert.equal(told.title, `Sold 0.027 B3 for 2${G}642 rUSD`);
  assert.equal(told.detail, "That is after the seller's fee of 2 rUSD.");
  assert.equal(told.meta, `at 97${G}000 to 98${G}000 rUSD per B3`);
  assert.equal(told.tone, "sell");
  assert.equal(told.icon, "arrow-up");
  assert.equal(told.result, "success");
  // A fill reported without its fee counts as none, and says nothing of one.
  const bare = tell(included(place("sell", "ioc", 98, 5), "FILLED", true, [fill("sell", 98, 5)]));
  assert.equal(bare.title, "Sold 0.005 B3 for 490 rUSD");
  assert.equal(bare.detail, "");
});

test("an order that traded in part says first what traded, then what did not", () => {
  // The offers were gone when it arrived: the rest of an immediate order is
  // dropped, and the result is a warning, not a success.
  const dropped = tell(included(place("buy", "ioc", 101, 20), "IOC_DONE", true, [fill("buy", 101, 10)]));
  assert.equal(dropped.title, `Bought 0.01 B3 for 1${G}010 rUSD`);
  assert.equal(dropped.detail, "The other 0.01 B3 was not bought: nothing more was on offer at your price. What it would have cost stays yours.");
  assert.equal(dropped.meta, `at 101${G}000 rUSD per B3, of 0.02 B3 asked`);
  assert.equal(dropped.result, "warning");
  const kept = tell(included(place("sell", "ioc", 98, 20), "IOC_DONE", true, [fill("sell", 98, 5, "1")]));
  assert.equal(kept.title, "Sold 0.005 B3 for 489 rUSD");
  assert.equal(kept.detail, "That is after the seller's fee of 1 rUSD. The other 0.015 B3 was not sold: nothing more was on offer at your price. It stays yours.");
  assert.equal(kept.result, "warning");

  // The rest of a limit order went into the book: that is what was asked.
  const resting = included(place("buy", "gtc", 101, 10), "PARTIAL_RESTING", true, [fill("buy", 101, 4)]);
  const waits = tell(resting, (label, m) => `(${label} in ${m.name})`);
  assert.equal(waits.title, "Bought 0.004 B3 for 404 rUSD");
  assert.equal(waits.detail, `The other 0.006 B3 went into the order book at 101${G}000 rUSD per B3. (a1 in B3/rUSD)`);
  assert.equal(waits.meta, `at 101${G}000 rUSD per B3, the rest went into the book`);
  assert.equal(waits.result, "success");
  // Without the page's knowledge of how the order stands, nothing is added.
  assert.equal(tell(resting).detail, `The other 0.006 B3 went into the order book at 101${G}000 rUSD per B3.`);

  // The engine stopped it with a refusal code after some of it traded: the
  // trades stand, and they are told first.
  const capped = tell(included(place("buy", "ioc", 110, 50), "REJ_FILL_CAP", false, [fill("buy", 101, 30)]));
  assert.equal(capped.title, `Bought 0.03 B3 for 3${G}030 rUSD`);
  assert.equal(capped.detail, "The other 0.02 B3 was stopped, because it reached the most trades the network allows for one order. What it would have cost stays yours.");
  assert.equal(capped.result, "warning");
  assert.equal(capped.tone, "buy");
});

test("an order that went into the book is told in the past tense", () => {
  const row = included(place("buy", "gtc", 99, 5), "RESTING", true);
  assert.deepEqual(tell(row), {
    title: "Placed an order to buy 0.005 B3",
    meta: `at 99${G}000 rUSD per B3`,
    detail: `It went into the order book at 99${G}000 rUSD per B3, to trade when someone sells at that price.`,
    tone: "mine",
    icon: "book",
    result: "success",
  });
  const sell = tell(included(place("sell", "post_only", 105, 5), "RESTING", true), () => "It is still waiting.");
  assert.equal(sell.title, "Placed an order to sell 0.005 B3");
  assert.equal(sell.detail, `It went into the order book at 105${G}000 rUSD per B3, to trade when someone buys at that price. It is still waiting.`);
});

test("an order that did nothing says that nothing moved", () => {
  const buy = tell(included(place("buy", "ioc", 101, 5), "IOC_DONE", true));
  assert.equal(buy.title, "Nothing was bought");
  assert.equal(buy.meta, "Nobody was selling at your price any more");
  assert.equal(buy.detail, "Nobody was selling at your price any more. Nothing was spent.");
  assert.equal(buy.result, "warning");
  const sell = tell(included(place("sell", "ioc", 98, 5), "IOC_DONE", true));
  assert.equal(sell.title, "Nothing was sold");
  assert.equal(sell.detail, "Nobody was buying at your price any more. Nothing left your account.");

  // Refused outright: what the network said, and that nothing moved.
  const crossed = tell(included(place("buy", "post_only", 103, 5), "REJ_WOULD_CROSS", false));
  assert.equal(crossed.title, "The order was refused");
  assert.equal(crossed.meta, "Refused by the network");
  assert.equal(crossed.detail, "The network refused it, because it could have traded at once, and a post-only order must wait in the book. Nothing was spent.");
  assert.equal(crossed.result, "danger");
  assert.equal(crossed.tone, "bad");
  const poor = tell(included(place("sell", "ioc", 98, 5), "REJ_INSUFFICIENT_AVAILABLE", false));
  assert.equal(poor.title, "Selling 0.005 B3 was refused");
  assert.equal(poor.detail, "The network refused it, because the account did not have enough available for it. Nothing left your account.");
  // A code this version does not know is shown as it is, not guessed at.
  assert.equal(tell(included(place("buy", "ioc", 101, 5), "REJ_NEW", false)).detail, "The network refused it, because the network answered REJ_NEW. Nothing was spent.");
});

test("cancelling says what was cancelled and that its funds are back", () => {
  const placed = place("buy", "gtc", 99, 5);
  assert.deepEqual(tell(included({ action: "cancel", order: "ef".repeat(32), placed }, "CANCELLED", true)), {
    title: "Order cancelled",
    meta: `Buy 0.005 B3 at 99${G}000 rUSD per B3`,
    detail: `Buy 0.005 B3 at 99${G}000 rUSD per B3. What it held is available again.`,
    tone: "ok",
    icon: "check",
    result: "success",
  });
  // An order this copy of B3 Hive did not place itself.
  const unknown = tell(included({ action: "cancel", order: "ef".repeat(32), placed: null }, "CANCELLED", true));
  assert.equal(unknown.meta, "");
  assert.equal(unknown.detail, "What it held is available again.");
  const all = tell(included({ action: "cancel_all" }, "CANCELLED", true));
  assert.equal(all.title, "All orders cancelled");
  assert.equal(all.detail, "What they held is available again.");

  // It had already traded or was already cancelled: not an error.
  for (const code of ["REJ_ORDER_CLOSED", "REJ_UNKNOWN_ORDER"]) {
    const late = tell(included({ action: "cancel", order: "ef".repeat(32), placed }, code, false));
    assert.equal(late.title, "There was nothing to cancel");
    assert.equal(late.detail, "That order was already filled or cancelled.");
    assert.equal(late.result, "warning");
  }
  const refused = tell(included({ action: "cancel", order: "ef".repeat(32), placed }, "REJ_NOT_OWN_ORDER", false));
  assert.equal(refused.title, "The cancellation was refused");
  assert.equal(refused.detail, "The network refused it, because the order belongs to another account.");
  assert.equal(refused.result, "danger");
});

test("sending says how much went to whom, or that nothing left", () => {
  assert.deepEqual(tell(included(transfer, "TRANSFERRED", true)), {
    title: "Sent 100 rUSD",
    meta: "To account abababab…ababab",
    detail: "To account abababab…ababab.",
    tone: "ok",
    icon: "arrow-up",
    result: "success",
  });
  const refused = tell(included(transfer, "REJ_NO_RECIPIENT", false));
  assert.equal(refused.title, "100 rUSD was not sent");
  assert.equal(refused.detail, "The network refused it, because the account it was sent to does not exist on this network yet. Nothing left your account.");
  assert.equal(refused.result, "danger");
  // Anything else falls back to the code's own label.
  const other = tell(included({ action: "other", kind: 9 }, "DEPOSIT_CREDITED", true));
  assert.equal(other.title, "Deposit credited");
  assert.equal(other.result, "success");
});

// ---- the simple card -------------------------------------------------------

function asking(side: "buy" | "sell", want: Asking["want"], have: bigint, levels: Level[], more: Partial<Asking> = {}): Asking {
  const buy = side === "buy";
  return {
    m: M,
    buy,
    pay: buy ? RUSD : B3,
    have,
    problem: "",
    want,
    plan: want ? plan(side, want, levels, have, M, 100) : null,
    offered: levels.length > 0,
    blocked: "",
    ...more,
  };
}

test("the button says what is in the way before anything about the amount", () => {
  const want = { of: "base", atoms: 5_000_000n } as const;
  assert.deepEqual(advise(asking("buy", want, RICH, ASKS, { blocked: "review" })), { go: "Waiting for your approval", enabled: false, hint: [] });
  assert.deepEqual(advise(asking("buy", want, RICH, ASKS, { blocked: "network" })), { go: "Waiting for the network", enabled: false, hint: [] });
  assert.deepEqual(advise(asking("buy", want, RICH, ASKS, { blocked: "prices" })), { go: "Waiting for live prices", enabled: false, hint: [] });

  // Nobody on the other side: an immediate order cannot work, and the page
  // points at the view where an order can wait.
  const leave = ["You can leave an order waiting at a price of your own instead. ", { link: "Open Pro", act: "pro" }];
  assert.deepEqual(advise(asking("buy", want, RICH, [])), { go: "Nobody is selling B3 right now", enabled: false, hint: leave });
  assert.deepEqual(advise(asking("sell", want, RICH, [])), { go: "Nobody is buying B3 right now", enabled: false, hint: leave });

  // Nothing typed, or something that is not an amount.
  assert.deepEqual(advise(asking("buy", null, RICH, ASKS)), { go: "Enter an amount", enabled: false, hint: [] });
  assert.deepEqual(advise(asking("buy", null, RICH, ASKS, { problem: "Use a dot for decimals, like 0.5." })), {
    go: "Enter an amount",
    enabled: false,
    hint: ["Use a dot for decimals, like 0.5."],
  });
});

test("an amount that cannot trade as typed is never sent, and the one that can is offered", () => {
  // Less than one step.
  assert.deepEqual(advise(asking("buy", { of: "quote", atoms: 50n }, RICH, ASKS)), {
    go: "That amount is too small",
    enabled: false,
    hint: ["The smallest amount is 0.001 B3, about 101 rUSD right now. ", { link: "Use 0.001 B3", act: "use", lots: 1n }],
  });
  assert.deepEqual(advise(asking("sell", { of: "base", atoms: 999_999n }, RICH, BIDS)), {
    go: "That amount is too small",
    enabled: false,
    hint: ["The smallest amount is 0.001 B3, about 98 rUSD right now. ", { link: "Use 0.001 B3", act: "use", lots: 1n }],
  });

  // More than the account holds: what it can pay for instead.
  assert.deepEqual(advise(asking("buy", { of: "quote", atoms: 5000n }, 1000n, ASKS)), {
    go: "Not enough rUSD",
    enabled: false,
    hint: [`You have 1${G}000 rUSD. `, { link: "Buy 0.009 B3 instead", act: "use", lots: 9n }],
  });
  assert.deepEqual(advise(asking("sell", { of: "base", atoms: 27_000_000n }, 3_999_999n, BIDS)), {
    go: "Not enough B3",
    enabled: false,
    hint: ["You have 0.003999999 B3. ", { link: "Sell 0.003 B3 instead", act: "use", lots: 3n }],
  });
  // More than the account holds, which itself would buy more than is for
  // sale: both are said, so the amount offered does not look like all the
  // balance buys.
  assert.deepEqual(advise(asking("buy", { of: "quote", atoms: 100_000_000n }, 500_000n, ASKS)), {
    go: "Not enough rUSD",
    enabled: false,
    hint: [`You have 500${G}000 rUSD, and only 0.139 B3 is for sale right now. `, { link: "Buy 0.139 B3 instead", act: "use", lots: 139n }],
  });
  // Not even one step: the only way on is more funds.
  assert.deepEqual(advise(asking("buy", { of: "quote", atoms: 5000n }, 50n, ASKS)), {
    go: "Not enough rUSD",
    enabled: false,
    hint: ["You have 50 rUSD. ", { link: "Add funds", act: "funds" }],
  });

  // More than the other side offers.
  assert.deepEqual(advise(asking("buy", { of: "base", atoms: 500_000_000n }, RICH, ASKS)), {
    go: "Not that much is for sale",
    enabled: false,
    hint: ["Only 0.139 B3 is for sale right now. ", { link: "Buy 0.139 B3 instead", act: "use", lots: 139n }],
  });
  assert.deepEqual(advise(asking("sell", { of: "base", atoms: 100_000_000n }, RICH, BIDS)), {
    go: "Not that much is wanted",
    enabled: false,
    hint: ["Buyers want only 0.039 B3 right now. ", { link: "Sell 0.039 B3 instead", act: "use", lots: 39n }],
  });
});

test("the button names the amount the order is for, and the hint says where that differs from what was typed", () => {
  // Exactly what was typed.
  assert.deepEqual(advise(asking("buy", { of: "base", atoms: 5_000_000n }, RICH, ASKS)), { go: "Buy 0.005 B3", enabled: true, hint: [] });
  assert.deepEqual(advise(asking("sell", { of: "base", atoms: 27_000_000n }, 50_000_000n, BIDS)), { go: "Sell 0.027 B3", enabled: true, hint: [] });
  // An amount between two steps.
  assert.deepEqual(advise(asking("buy", { of: "base", atoms: 5_999_999n }, RICH, ASKS)), {
    go: "Buy 0.005 B3",
    enabled: true,
    hint: ["B3 trades in steps of 0.001 B3, so this is for 0.005 B3."],
  });
  // A budget that does not divide into steps: four lots at 101 set 404 aside.
  assert.deepEqual(advise(asking("buy", { of: "quote", atoms: 500n }, RICH, ASKS)), {
    go: "Buy 0.004 B3",
    enabled: true,
    hint: ["It uses at most 404 rUSD of that, because B3 trades in steps of 0.001 B3. The rest stays yours."],
  });
  // A budget that is used in full needs no word.
  assert.deepEqual(advise(asking("buy", { of: "quote", atoms: 505n }, RICH, ASKS)), { go: "Buy 0.005 B3", enabled: true, hint: [] });
  // Selling for an amount: the lots that bring at least that, and what
  // they bring when it is more.
  assert.deepEqual(advise(asking("sell", { of: "quote", atoms: 98n }, RICH, BIDS)), {
    go: "Sell 0.002 B3",
    enabled: true,
    hint: ["That takes 0.002 B3, which brings about 195 rUSD."],
  });
  assert.deepEqual(advise(asking("sell", { of: "quote", atoms: 97n }, RICH, BIDS)), { go: "Sell 0.001 B3", enabled: true, hint: [] });
});
