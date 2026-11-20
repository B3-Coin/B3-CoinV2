// Run with: node --experimental-strip-types --test web/test/
import assert from "node:assert/strict";
import { test } from "node:test";

import { baseAtoms, plan, type Asset, type Level, type Market } from "../src/fm2fmt.ts";
import {
  advise,
  numberProblem,
  ordered,
  outlook,
  question,
  reachNote,
  size,
  sliderNote,
  tell,
  throughNote,
  type Advice,
  type Asking,
  type Drafted,
  type Fact,
  type Fill,
  type Intent,
  type Line,
  type Place,
  type Row,
} from "../src/fm2words.ts";

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
  // The engine rounds the fee up once for the whole order, on what its
  // fills bring together (src/flowmesh2/core/fee_math.h): 0.01% of 2 450 is
  // 1, and the second fill adds nothing to it.
  const told = tell(included(place("sell", "ioc", 97, 27), "FILLED", true, [fill("sell", 98, 25, "1"), fill("sell", 97, 2, "0")]));
  assert.equal(told.title, `Sold 0.027 B3 for 2${G}643 rUSD`);
  assert.equal(told.detail, "That is after the seller's fee of 1 rUSD.");
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
    detail: `It went into the order book at 99${G}000 rUSD per B3, to wait for a seller who accepts that price.`,
    tone: "mine",
    icon: "book",
    result: "success",
  });
  const sell = tell(included(place("sell", "post_only", 105, 5), "RESTING", true), () => "It is still waiting.");
  assert.equal(sell.title, "Placed an order to sell 0.005 B3");
  assert.equal(sell.detail, `It went into the order book at 105${G}000 rUSD per B3, to wait for a buyer who accepts that price. It is still waiting.`);
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
    detail: `Buy 0.005 B3 at 99${G}000 rUSD per B3. What it set aside is available again.`,
    tone: "ok",
    icon: "check",
    result: "success",
  });
  // An order this copy of B3 Hive did not place itself.
  const unknown = tell(included({ action: "cancel", order: "ef".repeat(32), placed: null }, "CANCELLED", true));
  assert.equal(unknown.meta, "");
  assert.equal(unknown.detail, "What it set aside is available again.");
  const all = tell(included({ action: "cancel_all" }, "CANCELLED", true));
  assert.equal(all.title, "All orders cancelled");
  assert.equal(all.detail, "What they set aside is available again.");
  // What it gave back is named from the confirmed change of the balances:
  // for an order that had traded in part, less than it first set aside.
  const back = { ...included({ action: "cancel", order: "ef".repeat(32), placed }, "CANCELLED", true), deltas: [{ asset: RUSD, available: "297", reserved: "-297" }] } as Row;
  assert.equal(tell(back).detail, `Buy 0.005 B3 at 99${G}000 rUSD per B3. The 297 rUSD it still set aside is available again.`);
  const both = {
    ...included({ action: "cancel_all" }, "CANCELLED", true),
    deltas: [
      { asset: RUSD, available: "297", reserved: "-297" },
      { asset: B3, available: "5000000", reserved: "-5000000" },
    ],
  } as Row;
  assert.equal(tell(both).detail, "The 297 rUSD and 0.005 B3 they still set aside are available again.");

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
  const p = want ? plan(side, want, levels, have, M, 100) : null;
  // As the page does it: what one piece more would take, for a budget.
  const up = p && p.lots > 0n && p.short === null && buy && want?.of === "quote" ? plan(side, { of: "base", atoms: baseAtoms(p.lots + 1n, M) }, levels, have, M, 100) : null;
  return {
    m: M,
    buy,
    pay: buy ? RUSD : B3,
    have,
    problem: "",
    want,
    plan: p,
    more: up && up.short === null ? up : null,
    best: levels.length ? BigInt(levels[0].price_ticks) : 0n,
    first: levels.length ? BigInt(levels[0].lots) : 0n,
    other: 0n,
    own: false,
    blocked: "",
    feePpm: 100,
    ...more,
  };
}

const ASK_NOW = [{ key: "Price now", value: `101${G}000 rUSD per B3` }];
const BID_NOW = [{ key: "Price now", value: `98${G}000 rUSD per B3` }];
/** A button that is off, with nothing else to say but the price. */
const off = (go: string, hint: Advice["hint"] = [], lines: Line[] = ASK_NOW): Advice => ({ go, enabled: false, lines, promise: "", hint, warn: [] });

test("the button says what is in the way before anything about the amount", () => {
  const want = { of: "base", atoms: 5_000_000n } as const;
  assert.deepEqual(advise(asking("buy", want, RICH, ASKS, { blocked: "review" })), off("Waiting for your approval"));
  // One request waits at a time: a form says so when it is not its own.
  assert.deepEqual(advise(asking("buy", want, RICH, ASKS, { blocked: "elsewhere" })), off("Another request waits for your approval"));
  assert.deepEqual(advise(asking("buy", want, RICH, ASKS, { blocked: "network" })), off("Waiting for the network"));
  assert.deepEqual(advise(asking("buy", want, RICH, ASKS, { blocked: "prices" })), off("Waiting for live prices"));

  // Nobody on the other side: an immediate order cannot work, and the page
  // points at the view where an order can wait.
  const leave = ["You can leave an order waiting at a price of your own instead. ", { link: "Open Pro", act: "pro" } as const];
  assert.deepEqual(advise(asking("buy", want, RICH, [])), off("Nobody is selling B3 right now", leave, []));
  assert.deepEqual(advise(asking("sell", want, RICH, [])), off("Nobody is buying B3 right now", leave, []));

  // Nothing typed, or something that is not an amount.
  assert.deepEqual(advise(asking("buy", null, RICH, ASKS)), off("Enter an amount"));
  assert.deepEqual(advise(asking("sell", null, RICH, BIDS)), off("Enter an amount", [], BID_NOW));
  assert.deepEqual(advise(asking("buy", null, RICH, ASKS, { problem: "Use a dot for decimals, like 0.5." })), off("Enter an amount", ["Use a dot for decimals, like 0.5."]));
});

test("an amount that cannot trade as typed is never sent, and the one that can is offered", () => {
  // Less than one piece.
  assert.deepEqual(
    advise(asking("buy", { of: "quote", atoms: 50n }, RICH, ASKS)),
    off("That amount is too small", ["The smallest amount is 0.001 B3, about 101 rUSD right now. ", { link: "Use 0.001 B3", act: "use", lots: 1n }]),
  );
  assert.deepEqual(
    advise(asking("sell", { of: "base", atoms: 999_999n }, RICH, BIDS)),
    off("That amount is too small", ["The smallest amount is 0.001 B3, about 98 rUSD right now. ", { link: "Use 0.001 B3", act: "use", lots: 1n }], BID_NOW),
  );

  // More than the account holds: what it can pay for instead.
  assert.deepEqual(
    advise(asking("buy", { of: "quote", atoms: 5000n }, 1000n, ASKS)),
    off("Not enough rUSD", [`You have 1${G}000 rUSD, enough for 0.009 B3. `, { link: "Change to 0.009 B3", act: "use", lots: 9n }]),
  );
  assert.deepEqual(
    advise(asking("sell", { of: "base", atoms: 27_000_000n }, 3_999_999n, BIDS)),
    off("Not enough B3", ["You have 0.003999999 B3. ", { link: "Change to 0.003 B3", act: "use", lots: 3n }], BID_NOW),
  );
  // More than the account holds, which itself would buy more than is for
  // sale: both are said, so the amount offered does not look like all the
  // balance buys.
  assert.deepEqual(
    advise(asking("buy", { of: "quote", atoms: 100_000_000n }, 500_000n, ASKS)),
    off("Not enough rUSD", [`You have 500${G}000 rUSD, and only 0.139 B3 is for sale right now. `, { link: "Change to 0.139 B3", act: "use", lots: 139n }]),
  );
  // Not even one piece: the only way on is more funds.
  assert.deepEqual(advise(asking("buy", { of: "quote", atoms: 5000n }, 50n, ASKS)), off("Not enough rUSD", ["You have 50 rUSD. ", { link: "Get test funds", act: "funds" }]));

  // More than the other side offers.
  assert.deepEqual(
    advise(asking("buy", { of: "base", atoms: 500_000_000n }, RICH, ASKS)),
    off("Not that much is for sale", ["Only 0.139 B3 is for sale right now. ", { link: "Change to 0.139 B3", act: "use", lots: 139n }]),
  );
  assert.deepEqual(
    advise(asking("sell", { of: "base", atoms: 100_000_000n }, RICH, BIDS)),
    off("Not that much is wanted", ["Buyers want only 0.039 B3 right now. ", { link: "Change to 0.039 B3", act: "use", lots: 39n }], BID_NOW),
  );
});

// "Less" alone would leave open whether it can be nothing, and whether
// the rest keeps waiting: both are said.
const ARRIVES = "Your order arrives in about a second.";
const BUY_PROMISE = `${ARRIVES} If sellers are gone by then, you get less or nothing and pay less, never more. Nothing is left waiting.`;
const NO_FEE = { key: "Fee", value: "None for buying" };

test("a buy at one price states its cost plainly, on the button too", () => {
  // Every piece trades at the best price, and the order's own limit is that
  // price: the cost is certain, so it is said without "about".
  assert.deepEqual(advise(asking("buy", { of: "base", atoms: 5_000_000n }, RICH, ASKS)), {
    go: "Buy 0.005 B3 for 505 rUSD",
    enabled: true,
    lines: [{ key: "You pay", value: "505 rUSD" }, { key: "Price", value: `101${G}000 rUSD per B3` }, NO_FEE],
    promise: BUY_PROMISE,
    hint: [],
    warn: [],
  });
  // An amount between two pieces.
  const between = advise(asking("buy", { of: "base", atoms: 5_999_999n }, RICH, ASKS));
  assert.equal(between.go, "Buy 0.005 B3 for 505 rUSD");
  assert.deepEqual(between.hint, ["B3 is traded in pieces of 0.001 B3, so this is for 0.005 B3."]);
  // One of the orders it would meet is the account's own.
  assert.deepEqual(advise(asking("buy", { of: "base", atoms: 5_000_000n }, RICH, ASKS, { own: true })).warn, [
    "One of the orders this would trade with is your own. Trading with yourself changes nothing, and the seller's fee is still taken.",
  ]);
});

test("a budget that does not come out even says where the rest goes", () => {
  // 500 rUSD pays for four pieces at 101; the fifth would take 505.
  const said = advise(asking("buy", { of: "quote", atoms: 500n }, RICH, ASKS));
  assert.equal(said.go, "Buy 0.004 B3 for 404 rUSD");
  assert.deepEqual(said.lines[0], { key: "You pay", value: "404 rUSD" });
  // The offer of one piece more is a change of the amount, and it says
  // what that would cost: more than the 500 typed.
  assert.deepEqual(said.hint, [
    "Your 500 rUSD buys 0.004 B3 for 404 rUSD. The other 96 rUSD stays yours, because B3 is traded in pieces of 0.001 B3. ",
    "One piece more would cost 505 rUSD in all. ",
    { link: "Change to 0.005 B3", act: "use", lots: 5n },
  ]);
  // The piece more is offered only when the balance pays for it.
  assert.deepEqual(advise(asking("buy", { of: "quote", atoms: 500n }, 500n, ASKS)).hint, [
    "Your 500 rUSD buys 0.004 B3 for 404 rUSD. The other 96 rUSD stays yours, because B3 is traded in pieces of 0.001 B3.",
  ]);
  // A budget that is used in full needs no word.
  const even = advise(asking("buy", { of: "quote", atoms: 505n }, RICH, ASKS));
  assert.equal(even.go, "Buy 0.005 B3 for 505 rUSD");
  assert.deepEqual(even.hint, []);
});

test("a buy that reaches past the best price is an estimate, with its ceiling beside it", () => {
  // 34 pieces at 101 and 2 at 103: 3 640 expected, 36 x 103 set aside.
  assert.deepEqual(advise(asking("buy", { of: "base", atoms: 36_000_000n }, RICH, ASKS)), {
    go: `Buy 0.036 B3 for about 3${G}640 rUSD`,
    enabled: true,
    lines: [{ key: "You pay", value: `about 3${G}640 rUSD`, note: `never more than 3${G}708 rUSD` }, { key: "Price", value: `about 101${G}111 rUSD per B3` }, NO_FEE],
    promise: BUY_PROMISE,
    // Two price steps are not worth a warning: a sentence says what it costs.
    hint: ["Only 0.034 B3 is for sale at the best price, so the rest costs more: about 4 rUSD more in all."],
    warn: [],
  });
  // A budget across two prices: the rest is not a number the order fixes.
  const budget = advise(asking("buy", { of: "quote", atoms: 3700n }, RICH, ASKS));
  assert.equal(budget.go, `Buy 0.035 B3 for about 3${G}537 rUSD`);
  // 35 pieces at the highest price reached are 3 605, and 36 would be 3 708.
  assert.equal(budget.lines[0].note, `never more than 3${G}605 rUSD`);
  // One limit is named, on the line above. What is left is no fixed sum
  // here, so the line gives the reason it is left: a piece more could
  // pass the budget. The offer of that piece ends the line.
  assert.deepEqual(budget.hint, [
    `Your 3${G}700 rUSD buys 0.035 B3 for about 3${G}537 rUSD. `,
    "Only 0.034 B3 is for sale at the best price, so the rest costs more: about 2 rUSD more in all. ",
    `One piece more could cost more than your 3${G}700 rUSD, so what is left of it stays yours. `,
    { link: "Change to 0.036 B3", act: "use", lots: 36n },
  ]);
  // The reason stays when the piece more cannot be offered.
  assert.deepEqual(advise(asking("buy", { of: "quote", atoms: 3700n }, 3700n, ASKS)).hint, [
    `Your 3${G}700 rUSD buys 0.035 B3 for about 3${G}537 rUSD. `,
    "Only 0.034 B3 is for sale at the best price, so the rest costs more: about 2 rUSD more in all. ",
    `One piece more could cost more than your 3${G}700 rUSD, so what is left of it stays yours.`,
  ]);
  assert.ok(!budget.hint.some((part) => typeof part === "string" && /held back|never cost more/.test(part)));

  // Far past the best price: the warning, in the money it costs.
  const far = advise(asking("buy", { of: "base", atoms: 40_000_000n }, RICH, ASKS));
  assert.equal(far.go, `Buy 0.04 B3 for about 4${G}059 rUSD`);
  assert.equal(far.lines[0].note, `never more than 4${G}400 rUSD`);
  // It names its prices, says that the sum above has the difference in
  // already, and ends with the amount that avoids it and what that costs.
  assert.deepEqual(far.warn, [
    `Few sellers are waiting. Only 0.034 B3 is for sale at the best price, 101${G}000 rUSD per B3. The rest costs up to 110${G}000 rUSD per B3, so this costs about 19 rUSD more than at the best price. That is already counted above. `,
    `To pay the best price only, 3${G}434 rUSD in all: `,
    { link: "Change to 0.034 B3", act: "use", lots: 34n },
  ]);
  assert.deepEqual(far.hint, []);
  // With an order of the account's own in the way, both are said.
  const both = advise(asking("buy", { of: "base", atoms: 40_000_000n }, RICH, ASKS, { own: true })).warn;
  assert.equal(both.length, 4);
  assert.equal(both[0], "One of the orders this would trade with is your own. Trading with yourself changes nothing, and the seller's fee is still taken. ");
  // A budget that reaches that far is not offered a piece more beside it.
  // (45 pieces: 39 below 110 and 6 at it, 4 950 at most.)
  const farBudget = advise(asking("buy", { of: "quote", atoms: 5000n }, RICH, ASKS));
  assert.equal(farBudget.go, `Buy 0.045 B3 for about 4${G}609 rUSD`);
  assert.ok(String(farBudget.warn[0]).startsWith("Few sellers are waiting."));
  assert.deepEqual(farBudget.warn[2], { link: "Change to 0.034 B3", act: "use", lots: 34n });
  // It still says why some of the budget is left.
  assert.deepEqual(farBudget.hint, [
    `Your 5${G}000 rUSD buys 0.045 B3 for about 4${G}609 rUSD. `,
    `One piece more could cost more than your 5${G}000 rUSD, so what is left of it stays yours.`,
  ]);

  // No part of a line runs into the next, and no line ends with a space.
  for (const line of [budget.hint, far.warn, farBudget.hint, farBudget.warn, both, advise(asking("buy", { of: "quote", atoms: 500n }, RICH, ASKS)).hint]) {
    line.forEach((part, i) => assert.ok(typeof part !== "string" || part.endsWith(" ") === i < line.length - 1, String(part)));
  }
});

test("a sale states what it brings after the fee, and the least it can bring", () => {
  // All of it at the best bid: 5 x 98 less a fee of 1.
  assert.deepEqual(advise(asking("sell", { of: "base", atoms: 5_000_000n }, 50_000_000n, BIDS)), {
    go: "Sell 0.005 B3 for 489 rUSD",
    enabled: true,
    lines: [
      { key: "You receive", value: "489 rUSD" },
      { key: "Price", value: `98${G}000 rUSD per B3` },
      { key: "Fee", value: "1 rUSD, already subtracted above" },
    ],
    promise: `${ARRIVES} If buyers are gone by then, you sell less or nothing, and never for less than 98${G}000 rUSD per B3. Nothing is left waiting.`,
    hint: [],
    warn: [],
  });
  // 25 at 98 and 2 at 97 bring 2 644 less 1. At the limit all 27 would
  // bring 2 619 less 1: the least for all of it, as an amount.
  assert.deepEqual(advise(asking("sell", { of: "base", atoms: 27_000_000n }, 50_000_000n, BIDS)), {
    go: `Sell 0.027 B3 for about 2${G}643 rUSD`,
    enabled: true,
    lines: [
      { key: "You receive", value: `about 2${G}643 rUSD`, note: `at least 2${G}618 rUSD for all of it` },
      { key: "Price", value: `about 97${G}926 rUSD per B3` },
      { key: "Fee", value: "1 rUSD, already subtracted above" },
    ],
    promise: `${ARRIVES} If buyers are gone by then, you sell less or nothing, and never for less than 97${G}000 rUSD per B3. Nothing is left waiting.`,
    hint: ["Only 0.025 B3 is wanted at the best price, so the rest sells for less: about 2 rUSD less in all."],
    warn: [],
  });
  // Selling for an amount: the pieces that bring at least that.
  const forAmount = advise(asking("sell", { of: "quote", atoms: 98n }, RICH, BIDS));
  assert.equal(forAmount.go, "Sell 0.002 B3 for 195 rUSD");
  assert.deepEqual(forAmount.hint, ["That takes 0.002 B3, which brings 195 rUSD."]);
  assert.deepEqual(advise(asking("sell", { of: "quote", atoms: 97n }, RICH, BIDS)).hint, []);
  // Far below the best bid: the warning, in the money it loses.
  const far = advise(asking("sell", { of: "base", atoms: 30_000_000n }, 50_000_000n, BIDS));
  // (25 x 98 less a fee of 1 at the best price alone.)
  assert.deepEqual(far.warn, [
    `Few buyers are waiting. Only 0.025 B3 is wanted at the best price, 98${G}000 rUSD per B3. The rest sells for as little as 90${G}000 rUSD per B3, so this brings about 12 rUSD less than at the best price. That is already counted above. `,
    `To sell at the best price only, for 2${G}449 rUSD: `,
    { link: "Change to 0.025 B3", act: "use", lots: 25n },
  ]);
  assert.deepEqual(far.hint, []);
});

test("the card names the price of the other side beside its own", () => {
  // Buying costs more than selling brings: both prices are in sight.
  const buy = advise(asking("buy", { of: "base", atoms: 5_000_000n }, RICH, ASKS, { other: 98n }));
  assert.deepEqual(buy.lines[1], { key: "Price", value: `101${G}000 rUSD per B3`, note: `if you were selling: 98${G}000` });
  const sell = advise(asking("sell", { of: "base", atoms: 5_000_000n }, RICH, BIDS, { other: 101n }));
  assert.deepEqual(sell.lines[1], { key: "Price", value: `98${G}000 rUSD per B3`, note: `if you were buying: 101${G}000` });
  // Before an amount is typed, too.
  assert.deepEqual(advise(asking("buy", null, RICH, ASKS, { other: 98n })).lines, [{ key: "Price now", value: `101${G}000 rUSD per B3`, note: `if you were selling: 98${G}000` }]);
  // An estimate has its bound on the line above; its price line stays plain.
  assert.equal(advise(asking("buy", { of: "base", atoms: 36_000_000n }, RICH, ASKS, { other: 98n })).lines[1].note, undefined);
  // Nobody on the card's own side: nothing to name.
  assert.equal(advise(asking("buy", { of: "base", atoms: 5_000_000n }, RICH, ASKS)).lines[1].note, undefined);
});

test("no number is stated as certain unless the order's own limit makes it so", () => {
  // Whatever is asked, a plan that reaches past the best price says
  // "about" on the button and in the receipt, and gives the bound.
  for (const lots of [35n, 36n, 39n, 40n, 100n, 139n]) {
    const said = advise(asking("buy", { of: "base", atoms: baseAtoms(lots, M) }, RICH, ASKS));
    assert.ok(said.go.includes(" for about "), said.go);
    assert.ok(said.lines[0].value.startsWith("about "));
    assert.ok(said.lines[0].note?.startsWith("never more than "));
  }
  for (const lots of [26n, 29n, 30n, 39n]) {
    const said = advise(asking("sell", { of: "base", atoms: baseAtoms(lots, M) }, RICH, BIDS));
    assert.ok(said.go.includes(" for about "), said.go);
    assert.ok(said.lines[0].note?.startsWith("at least "));
  }
  // And one that stays at the best price never hedges.
  for (const lots of [1n, 2n, 34n]) {
    const said = advise(asking("buy", { of: "base", atoms: baseAtoms(lots, M) }, RICH, ASKS));
    assert.ok(!said.go.includes("about"), said.go);
    assert.equal(said.lines[0].note, undefined);
  }
});

// ---- the order form --------------------------------------------------------

test("a limit order's outlook says whether it waits, trades, or some of each", () => {
  // Below the lowest ask: it waits, or does nothing when it may not wait.
  const waits = { text: "Right now: all of it waits in the order book.", refused: false };
  assert.deepEqual(outlook("buy", "gtc", 100n, 10n, ASKS, M), waits);
  assert.deepEqual(outlook("buy", "post_only", 100n, 10n, ASKS, M), waits);
  assert.equal(outlook("buy", "ioc", 100n, 10n, ASKS, M).text, "Right now: nothing would trade, because nobody sells at this price or lower.");
  assert.equal(outlook("sell", "ioc", 99n, 10n, BIDS, M).text, "Right now: nothing would trade, because nobody buys at this price or higher.");
  assert.deepEqual(outlook("buy", "gtc", 101n, 10n, [], M), waits);
  assert.deepEqual(outlook("buy", "post_only", 101n, 10n, [], M), waits);
  // At or through it: it trades, at the book's prices.
  assert.equal(outlook("buy", "gtc", 101n, 10n, ASKS, M).text, `Right now: all of it trades at once, at 101${G}000 rUSD, for 1${G}010 rUSD.`);
  assert.equal(outlook("buy", "ioc", 110n, 10n, ASKS, M).text, `Right now: all of it trades at once, at 101${G}000 rUSD, for 1${G}010 rUSD.`);
  assert.equal(outlook("buy", "gtc", 103n, 36n, ASKS, M).text, `Right now: all of it trades at once, at about 101${G}111 rUSD, for 3${G}640 rUSD.`);
  // A sale's sum is what the trades bring; the fee comes off after.
  assert.equal(outlook("sell", "gtc", 97n, 27n, BIDS, M).text, `Right now: all of it trades at once, at about 97${G}926 rUSD, for 2${G}644 rUSD before the fee.`);
  // More than is offered up to its price: the rest waits, or is dropped.
  assert.equal(outlook("buy", "gtc", 101n, 40n, ASKS, M).text, `Right now: 0.034 B3 trades at once, at 101${G}000 rUSD, for 3${G}434 rUSD, and the other 0.006 B3 waits.`);
  assert.equal(outlook("buy", "ioc", 101n, 40n, ASKS, M).text, `Right now: 0.034 B3 trades at once, at 101${G}000 rUSD, for 3${G}434 rUSD, and the other 0.006 B3 is dropped.`);
  assert.equal(outlook("sell", "gtc", 98n, 30n, BIDS, M).text, `Right now: 0.025 B3 trades at once, at 98${G}000 rUSD, for 2${G}450 rUSD before the fee, and the other 0.005 B3 waits.`);
  // Only a refusal is marked as one.
  for (const tif of ["gtc", "ioc"]) assert.equal(outlook("buy", tif, 101n, 40n, ASKS, M).refused, false);
  // A post-only order that could trade is refused by the network.
  assert.deepEqual(outlook("buy", "post_only", 101n, 10n, ASKS, M), {
    text: `Right now: it would be refused, because it could trade at once. Post only needs 100${G}000 rUSD or lower.`,
    refused: true,
  });
  assert.deepEqual(outlook("sell", "post_only", 97n, 10n, BIDS, M), {
    text: `Right now: it would be refused, because it could trade at once. Post only needs 99${G}000 rUSD or higher.`,
    refused: true,
  });
});

// ---- the approval window ---------------------------------------------------

const drafted = (intent: Intent): Drafted => ({ chain: "regtest", domain: "fm2-regtest-1", account: "ab".repeat(32), intent, window_slots: 64, seats: 4, fee_ppm: 100 });
const keys = (facts: Fact[]): string[] => facts.map((f) => f.key);
const DETAILS = ["Trading account", "Valid for", "When you approve", "Network"];

test("the order form's notes on an order's reach say how much, how far and which way", () => {
  const buying = (lots: bigint) => plan("buy", { of: "base", atoms: baseAtoms(lots, M) }, ASKS, RICH, M, 100);
  const selling = (lots: bigint) => plan("sell", { of: "base", atoms: baseAtoms(lots, M) }, BIDS, RICH, M, 100);
  // An immediate buy of 0.04: 0.034 at the best price, 101, and the other
  // 0.006 at up to 110, which is 9 of 101 above it.
  assert.equal(reachNote("buy", buying(40n), 34n, M), `0.006 B3 of it costs more than the best price, up to 110${G}000 rUSD per B3: 8.91% above it.`);
  // One price step past the best one is under two percent: no note.
  assert.equal(reachNote("buy", buying(36n), 34n, M), "");
  assert.equal(reachNote("buy", buying(34n), 34n, M), "");
  // A sale of 0.03: 0.025 at 98, the rest down to 90.
  assert.equal(reachNote("sell", selling(30n), 25n, M), `0.005 B3 of it sells below the best price, down to 90${G}000 rUSD per B3: 8.16% below it.`);
  assert.equal(reachNote("sell", selling(27n), 25n, M), "");

  // A limit of the user's own far through the book: it trades at the
  // book's prices, and its own price is the furthest it may go, which is
  // up for a buy and down for a sale.
  assert.equal(throughNote("buy", 107n, 101n), "Your price is 5.94% above the lowest ask. It trades at the book's prices, up to yours.");
  assert.equal(throughNote("sell", 93n, 98n), "Your price is 5.10% below the highest bid. It trades at the book's prices, down to yours.");
  // Not far enough, at the best price, short of it, or against nothing.
  for (const [side, ticks, top] of [["buy", 106n, 101n], ["buy", 101n, 101n], ["buy", 90n, 101n], ["sell", 94n, 98n], ["sell", 120n, 98n], ["buy", 500n, 0n]] as const) {
    assert.equal(throughNote(side, ticks, top), "", `${side} ${ticks} ${top}`);
  }

  // The slider's whole, when the book holds less than the balance pays for.
  assert.equal(sliderNote("buy", 139n, M), "100% is 0.139 B3: all that is for sale right now.");
  assert.equal(sliderNote("sell", 39n, M), "100% is 0.039 B3: all that buyers want right now.");
});

test("the approval window asks about an immediate order with the bound its limit gives", () => {
  const buy = question(drafted(place("buy", "ioc", 101, 4)));
  assert.equal(buy.title, "Buy 0.004 B3 for at most 404 rUSD?");
  // "Up to" and "at most": neither amount is promised, and the page's
  // button may have said less than this bound.
  assert.equal(
    buy.explain,
    `You get up to 0.004 B3 and pay at most 404 rUSD. You pay less if sellers ask less than 101${G}000 rUSD per B3. If some of them are gone when your order arrives, you get less and pay less, never more. A trade cannot be undone.`,
  );
  assert.deepEqual(buy.facts, [
    { key: "You buy up to", value: "0.004 B3", hint: "Less if sellers are gone when your order arrives. Then you also pay less." },
    // The page may have said "about" a smaller sum: this one is the bound.
    { key: "You pay at most", value: "404 rUSD", hint: "The most it can cost, not an estimate. Less if sellers ask a lower price. Buyers pay no fee." },
    { key: "Price limit", value: `101${G}000 rUSD per B3`, hint: "It never pays a higher price than this." },
    { key: "If not all of it can trade at once", value: "The rest is not bought, and its cost stays yours" },
  ]);
  assert.equal(buy.approve, "Buy 0.004 B3");
  assert.equal(buy.reject, "Don't buy");
  // What is signed, to the integer, is under Details.
  assert.deepEqual(keys(buy.more), ["Market", "In lots and ticks", "Time in force", ...DETAILS]);
  assert.equal(buy.more[1].value, "4 lots at 101 ticks");
  assert.equal(buy.more[0].full, M.id);
  assert.equal(buy.more[3].full, "ab".repeat(32));

  // Ten pieces at 98 bring 980, less a fee of 1.
  const sell = question(drafted(place("sell", "ioc", 98, 10)));
  assert.equal(sell.title, "Sell 0.01 B3 for at least 979 rUSD?");
  assert.equal(
    sell.explain,
    `You sell up to 0.01 B3. For all of it you receive at least 979 rUSD, after the seller's fee, and more if buyers pay more than 98${G}000 rUSD per B3. If some of them are gone when your order arrives, you sell less, and never for less than 98${G}000 rUSD per B3. A trade cannot be undone.`,
  );
  assert.deepEqual(sell.facts, [
    { key: "You sell up to", value: "0.01 B3", hint: "Less if buyers are gone when your order arrives." },
    { key: "For all of it you receive at least", value: "979 rUSD", hint: "The least it can bring, not an estimate. After the seller's fee. More if buyers pay a higher price." },
    { key: "Seller's fee", value: "1 rUSD at this price", hint: "0.01% of what the sale brings, rounded up." },
    { key: "Price limit", value: `98${G}000 rUSD per B3`, hint: "It never sells at a lower price than this." },
    { key: "If not all of it can trade at once", value: "The rest is not sold and stays yours" },
  ]);
  assert.equal(sell.approve, "Sell 0.01 B3");
  assert.equal(sell.reject, "Don't sell");
});

test("the approval window says what an order that waits sets aside", () => {
  const buy = question(drafted(place("buy", "gtc", 99, 5)));
  assert.equal(buy.title, `Buy 0.005 B3 at 99${G}000 rUSD per B3?`);
  assert.equal(
    buy.explain,
    "Whatever can trade at once at this price or lower, trades. The rest waits in the order book for a seller who accepts your price, until you cancel it. While it waits, 495 rUSD is set aside from your balance. What has traded cannot be undone.",
  );
  // The form's own words for it, and no amount promised: an order that
  // waits may buy all, part or none, and its button places it.
  assert.deepEqual(keys(buy.facts), ["You buy up to", "You pay at most", "Price limit", "What cannot trade at once"]);
  assert.equal(buy.facts[0].hint, "Whenever sellers accept your price. That can be all of it, part of it, or none.");
  assert.equal(buy.facts[1].hint, "Set aside while the order waits. Buyers pay no fee.");
  assert.equal(buy.facts[3].value, "Waits until it trades or you cancel it");
  assert.equal(buy.approve, "Place the order");
  assert.equal(buy.reject, "Don't place it");

  const post = question(drafted(place("sell", "post_only", 105, 5)));
  assert.equal(post.title, `Sell 0.005 B3 at 105${G}000 rUSD per B3?`);
  assert.equal(
    post.explain,
    "It only waits in the order book, for a buyer who accepts your price, until you cancel it. If it could trade at once, the network refuses it and nothing happens. While it waits, 0.005 B3 is set aside from your balance.",
  );
  assert.deepEqual(keys(post.facts), ["You sell up to", "For all of it you receive at least", "Seller's fee", "Price limit", "If it could trade at once"]);
  assert.equal(post.facts[0].hint, "Whenever buyers accept your price. That can be all of it, part of it, or none.");
  assert.equal(post.facts[1].value, "524 rUSD"); // 5 x 105 less a fee of 1
  // An order that waits was never shown with an estimate to set it against.
  assert.equal(post.facts[1].hint, "After the seller's fee. More if buyers pay a higher price.");
  assert.equal(post.facts[4].value, "The network refuses all of it");
  assert.equal(post.approve, "Place the order");
});

test("the approval window states only what the order guarantees", () => {
  // It knows the order, not the book: no estimate may appear above Details.
  for (const tif of ["ioc", "gtc", "post_only"]) {
    for (const side of ["buy", "sell"] as const) {
      const q = question(drafted(place(side, tif, 103, 36)));
      const shown = [q.title, q.explain, ...q.facts.flatMap((f) => [f.key, f.value, f.hint ?? ""])].join(" ");
      assert.ok(!/\babout\b/.test(shown), `${side} ${tif}: ${shown}`);
      assert.ok(/at most|at least/.test(shown), `${side} ${tif}`);
    }
  }
});

test("the approval window asks about a cancellation, a transfer and what it cannot show", () => {
  const placed = place("buy", "gtc", 99, 5);
  const cancel = question(drafted({ action: "cancel", order: "ef".repeat(32), placed }));
  assert.equal(cancel.title, "Cancel this order?");
  assert.equal(
    cancel.explain,
    "The order stops waiting in the order book, and what it still sets aside becomes available again. That takes about a second: until the network confirms it, the order can still trade. Cancelling costs nothing.",
  );
  assert.deepEqual(cancel.facts, [{ key: "Order", value: `Buy 0.005 B3 at 99${G}000 rUSD per B3`, hint: "As it was placed. Part of it may have traded since." }]);
  assert.deepEqual(keys(cancel.more), ["Order number", ...DETAILS]);
  // Yes cancels and no keeps: neither button is a "no to a no".
  assert.equal(cancel.approve, "Cancel the order");
  assert.equal(cancel.reject, "Keep the order");
  // An order this copy of B3 Hive did not place is named by its number.
  assert.deepEqual(question(drafted({ action: "cancel", order: "ef".repeat(32), placed: null })).facts, [{ key: "Order", value: "efefefefef…efefef", full: "ef".repeat(32) }]);

  const all = question(drafted({ action: "cancel_all" }));
  assert.equal(all.title, "Cancel all your orders?");
  assert.deepEqual(all.facts, []);
  assert.ok(all.explain.endsWith("That takes about a second: until the network confirms it, the orders can still trade. Cancelling costs nothing."));
  assert.equal(all.approve, "Cancel all orders");
  assert.equal(all.reject, "Keep the orders");

  const send = question(drafted(transfer));
  assert.equal(send.title, "Send 100 rUSD?");
  assert.equal(send.explain, "It goes to another account on this test network, with no fee. It stays inside FlowMesh: nothing moves on the B3 chain. A transfer cannot be undone, so check the account number.");
  assert.deepEqual(send.facts, [
    { key: "You send", value: "100 rUSD" },
    { key: "To account", value: "ababababab…ababab", full: "ab".repeat(32) },
  ]);
  assert.equal(send.approve, "Send 100 rUSD");
  assert.equal(send.reject, "Don't send");

  // Never approve blind: an action this version cannot show has no button.
  const other = question(drafted({ action: "other", kind: 9 }));
  assert.equal(other.title, "This request cannot be shown");
  assert.equal(other.approve, "");
  assert.equal(other.reject, "Reject");
  assert.deepEqual(other.more, []);
});
