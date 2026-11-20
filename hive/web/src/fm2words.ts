// What the Trade page and the approval window say: the sentence a result
// becomes, what the simple card's button and the lines around it say in
// each state, what a limit order would do right now, and the question the
// approval window asks. Pure functions over the core's view models and the
// plan of fm2fmt.ts, with no page and no clock in them, so every sentence
// can be tested.
import {
  amountText,
  averagePrice,
  averageText,
  baseAtoms,
  decimals,
  feeRate,
  lotText,
  outcomeText,
  price,
  priceText,
  quoteAtoms,
  reach,
  refusal,
  sellerFee,
  shortId,
  tickText,
  tifText,
  unit,
  whole,
  type Asset,
  type Level,
  type Market,
  type Plan,
  type Want,
} from "./fm2fmt.ts";

// ---- what the core sends about an action (src-tauri/src/fm2/view.rs) --------

export type Amount = { asset: Asset; atoms: string };

export type Place = { action: "place"; market: Market; side: "buy" | "sell"; tif: string; price_ticks: string; lots: string; base: Amount; quote: Amount };
export type Intent =
  | Place
  | { action: "cancel"; order: string; placed: Intent | null }
  | { action: "cancel_all" }
  | { action: "transfer"; to: string; amount: Amount }
  | { action: "other"; kind: number };

export type Fill = { market: string; side: "buy" | "sell"; price_ticks: string; lots: string; quote: Amount; fee_atoms: string | null };

export type Included = {
  state: "included";
  height: string;
  index: number;
  code: number;
  code_name: string;
  accepted: boolean;
  remaining_lots: string;
  fills: Fill[];
  deltas: { asset: Asset; available: string; reserved: string }[];
};

/** One action this account signed, and what became of it. */
export type Row = {
  label: string;
  created: number;
  action_id: string;
  nonce: string;
  ref_slot: string;
  window_slots: number;
  intent: Intent;
} & ({ state: "open"; note: string } | Included | { state: "expired"; height: string });

// ---- small words -----------------------------------------------------------

/** An amount of a market's base asset, from its lots: "0.005 B3". */
export const size = (lots: bigint, m: Market): string => amountText(baseAtoms(lots, m), m.base);

/** Why a typed amount is not one, in words that say how to fix it. */
export function numberProblem(text: string, asset: Asset): string {
  if (text.includes(",")) return "Use a dot for decimals, like 0.5.";
  const places = decimals(asset);
  if (/^[\d\s]+\.\d+$/.test(text)) {
    return places === 0 ? `${unit(asset)} has no decimals on this network. Enter a whole number.` : `${unit(asset)} has at most ${places} decimals.`;
  }
  return "Enter a number, like 0.5 or 100.";
}

/** An order in a few words: "Buy 0.005 B3 at 99 000 rUSD per B3". */
export function ordered(i: Place): string {
  return `${i.side === "buy" ? "Buy" : "Sell"} ${size(BigInt(i.lots), i.market)} at ${priceText(BigInt(i.price_ticks), i.market)}`;
}

// ---- what became of something the user signed ------------------------------

export type Told = {
  /** One sentence: what happened. */
  title: string;
  /** A few words under it in a list. */
  meta: string;
  /** The same few words as a sentence, for where the list is not. */
  lead?: string;
  /** The rest, in full sentences. */
  detail: string;
  /** The colour of its mark in a list. */
  tone: "buy" | "sell" | "ok" | "bad" | "wait" | "mine" | "";
  icon: string;
  /** How its result reads: it worked, it did less than asked, it failed. */
  result: "success" | "warning" | "danger";
};

/** What became of an action, from its confirmed result alone.
 *
 *  A result is told by what it did, not by its code: an order that traded
 *  in part says what was bought or sold first and what was left out second,
 *  also when the engine stopped the rest with a refusal code. `standing`
 *  says how an order that went into the book stands now; the page knows,
 *  this function does not. */
export function tell(row: Row, standing: (label: string, m: Market) => string = () => ""): Told {
  const i = row.intent;
  if (row.state === "open") {
    const title =
      i.action === "place"
        ? i.tif === "ioc"
          ? `${i.side === "buy" ? "Buying" : "Selling"} ${size(BigInt(i.lots), i.market)}`
          : `Placing an order to ${i.side} ${size(BigInt(i.lots), i.market)}`
        : i.action === "cancel"
          ? "Cancelling an order"
          : i.action === "cancel_all"
            ? "Cancelling all orders"
            : i.action === "transfer"
              ? `Sending ${amountText(BigInt(i.amount.atoms), i.amount.asset)}`
              : "Something you signed";
    return { title, meta: "Waiting for the network to confirm it", detail: "", tone: "wait", icon: "clock", result: "warning" };
  }
  if (row.state === "expired") {
    const title =
      i.action === "place"
        ? i.tif === "ioc"
          ? `${size(BigInt(i.lots), i.market)} was not ${i.side === "buy" ? "bought" : "sold"}`
          : "The order was not placed"
        : i.action === "cancel"
          ? "The order was not cancelled"
          : i.action === "cancel_all"
            ? "The orders were not cancelled"
            : i.action === "transfer"
              ? `${amountText(BigInt(i.amount.atoms), i.amount.asset)} was not sent`
              : "It did not happen";
    return { title, meta: "The network did not take it in time", detail: "The network did not take it in time, and now it never can. Nothing changed in your account.", tone: "", icon: "alert", result: "warning" };
  }
  const why = refusal(row.code_name);
  if (i.action === "place") {
    const m = i.market;
    const buy = i.side === "buy";
    const asked = BigInt(i.lots);
    const done = row.fills.reduce((sum, f) => sum + BigInt(f.lots), 0n);
    const rest = asked > done ? asked - done : 0n;
    const untouched = buy ? "Nothing was spent." : "Nothing left your account.";
    if (done > 0n) {
      const gross = row.fills.reduce((sum, f) => sum + BigInt(f.quote.atoms), 0n);
      const fee = row.fills.reduce((sum, f) => sum + BigInt(f.fee_atoms ?? "0"), 0n);
      const prices = row.fills.map((f) => BigInt(f.price_ticks));
      const low = prices.reduce((a, b) => (b < a ? b : a));
      const high = prices.reduce((a, b) => (b > a ? b : a));
      const at = low === high ? `at ${priceText(low, m)}` : `at ${price(low, m)} to ${priceText(high, m)}`;
      const title = buy ? `Bought ${size(done, m)} for ${amountText(gross, m.quote)}` : `Sold ${size(done, m)} for ${amountText(gross - fee, m.quote)}`;
      const parts: string[] = [];
      if (!buy && fee > 0n) parts.push(`That is after the seller's fee of ${amountText(fee, m.quote)}.`);
      let meta = at;
      let result: Told["result"] = "success";
      if (rest > 0n) {
        const kept = buy ? "What it would have cost stays yours." : "It stays yours.";
        if (row.code_name === "PARTIAL_RESTING") {
          parts.push(`The other ${size(rest, m)} went into the order book at ${priceText(BigInt(i.price_ticks), m)}.`, standing(row.label, m));
          meta = `${at}, the rest went into the book`;
        } else if (row.accepted) {
          parts.push(`The other ${size(rest, m)} was not ${buy ? "bought" : "sold"}: nothing more was on offer at your price. ${kept}`);
          meta = `${at}, of ${size(asked, m)} asked`;
          result = "warning";
        } else {
          parts.push(`The other ${size(rest, m)} was stopped, because ${why}. ${kept}`);
          meta = `${at}, of ${size(asked, m)} asked`;
          result = "warning";
        }
      }
      return { title, meta, lead: `A${at.slice(1)}.`, detail: parts.filter(Boolean).join(" "), tone: buy ? "buy" : "sell", icon: buy ? "arrow-down" : "arrow-up", result };
    }
    if (row.code_name === "RESTING") {
      // What happened then, in the past tense: the row stays in the history
      // long after the order has traded or was cancelled.
      return {
        title: `Placed an order to ${i.side} ${size(asked, m)}`,
        meta: `at ${priceText(BigInt(i.price_ticks), m)}`,
        detail: [`It went into the order book at ${priceText(BigInt(i.price_ticks), m)}, to trade when someone ${buy ? "sells" : "buys"} at that price.`, standing(row.label, m)].filter(Boolean).join(" "),
        tone: "mine",
        icon: "book",
        result: "success",
      };
    }
    if (row.accepted) {
      const gone = `Nobody was ${buy ? "selling" : "buying"} at your price any more`;
      return { title: `Nothing was ${buy ? "bought" : "sold"}`, meta: gone, detail: `${gone}. ${untouched}`, tone: "", icon: "alert", result: "warning" };
    }
    return {
      title: i.tif === "ioc" ? `${buy ? "Buying" : "Selling"} ${size(asked, m)} was refused` : "The order was refused",
      meta: "Refused by the network",
      detail: `The network refused it, because ${why}. ${untouched}`,
      tone: "bad",
      icon: "alert",
      result: "danger",
    };
  }
  if (i.action === "cancel" || i.action === "cancel_all") {
    const all = i.action === "cancel_all";
    const what = !all && i.placed?.action === "place" ? ordered(i.placed) : "";
    if (row.accepted) {
      // What it gave back, from the confirmed change of the balances: for
      // an order that had traded in part, less than it first set aside.
      const freed = row.deltas.filter((d) => BigInt(d.available) > 0n).map((d) => amountText(BigInt(d.available), d.asset));
      const back = freed.length
        ? `The ${freed.join(" and ")} ${all ? "they" : "it"} still held ${freed.length > 1 ? "are" : "is"} available again.`
        : `What ${all ? "they" : "it"} held is available again.`;
      return {
        title: all ? "All orders cancelled" : "Order cancelled",
        meta: what,
        detail: `${what ? `${what}. ` : ""}${back}`,
        tone: "ok",
        icon: "check",
        result: "success",
      };
    }
    if (row.code_name === "REJ_ORDER_CLOSED" || row.code_name === "REJ_UNKNOWN_ORDER") {
      return { title: "There was nothing to cancel", meta: what, detail: "That order was already filled or cancelled.", tone: "", icon: "alert", result: "warning" };
    }
    return { title: "The cancellation was refused", meta: "Refused by the network", detail: `The network refused it, because ${why}.`, tone: "bad", icon: "alert", result: "danger" };
  }
  if (i.action === "transfer") {
    const what = amountText(BigInt(i.amount.atoms), i.amount.asset);
    const to = `To account ${shortId(i.to)}`;
    if (row.accepted) return { title: `Sent ${what}`, meta: to, detail: `${to}.`, tone: "ok", icon: "arrow-up", result: "success" };
    return { title: `${what} was not sent`, meta: "Refused by the network", detail: `The network refused it, because ${why}. Nothing left your account.`, tone: "bad", icon: "alert", result: "danger" };
  }
  return { title: outcomeText(row.code_name), meta: "", detail: "", tone: row.accepted ? "ok" : "bad", icon: row.accepted ? "check" : "alert", result: row.accepted ? "success" : "danger" };
}

// ---- the simple card: what its button says, and the lines around it --------

/** From how far past the best price a trade gets the yellow warning. Two
 *  price steps are 2% in the regtest market, so a lower bar would warn
 *  about almost every trade that leaves the best price; below it the same
 *  facts are a plain sentence under the card. */
export const WIDE_BPS = 500n;

/** Something the line under the card offers to do in one click. */
export type Offer = { link: string; act: "use"; lots: bigint } | { link: string; act: "funds" | "pro" };

/** One line of the card's receipt: what, how much, and a smaller word on
 *  how sure that is. */
export type Line = { key: string; value: string; note?: string };

export type Advice = {
  /** What the button says: exactly what it will do, with the money in it,
   *  or why it cannot. */
  go: string;
  enabled: boolean;
  /** What the order comes to, above the button. */
  lines: Line[];
  /** What holds whatever happens before the order arrives. */
  promise: string;
  /** The line under the card: words, and what it offers to do instead. */
  hint: (string | Offer)[];
  /** What deserves a second look before going on, and the way out of it
   *  as an offer at its end; empty for nothing. */
  warn: (string | Offer)[];
};

export type Asking = {
  m: Market;
  buy: boolean;
  /** The asset paid with, and how much of it the account can spend. */
  pay: Asset;
  have: bigint;
  /** Why the typed text is not an amount; empty when it is one, or empty. */
  problem: string;
  want: Want | null;
  plan: Plan | null;
  /** The plan for one piece more, when the book and the balance allow it:
   *  what to offer someone whose budget did not come out even. */
  more: Plan | null;
  /** The best price on the other side of the book, in ticks, and the lots
   *  waiting at it; zero when nobody is there. */
  best: bigint;
  first: bigint;
  /** The best price on the card's own side, in ticks: what the opposite
   *  trade would get right now. Zero when nobody is there. */
  other: bigint;
  /** One of the orders it would trade with is the account's own. */
  own: boolean;
  /** What stops every trade right now: a request already waiting for
   *  approval, too few validators, or no live prices. */
  blocked: "" | "review" | "elsewhere" | "network" | "prices";
  feePpm: number;
};

/** What a plan costs or brings, as the button and the receipt say it: the
 *  plain amount when every piece trades at one price, where the order's own
 *  limit guarantees it, and "about" with the estimate when it reaches
 *  further into the book. */
function money(p: Plan, buy: boolean, m: Market): string {
  const sure = p.limit === p.best;
  const atoms = buy ? (sure ? p.ceiling : p.gross) : p.gross - p.fee;
  return `${sure ? "" : "about "}${amountText(atoms, m.quote)}`;
}

/** The simple card's button, receipt, promise, hint and warning.
 *
 *  A plan for less than was asked is never sent as it is: the button stays
 *  off, says what is in the way, and the hint offers the amount that works,
 *  for the user to take or leave. A number is stated without "about" only
 *  where the order's own price limit makes it certain. */
/** What a form's button says while a request waits in the approval window:
 *  its own, or one made elsewhere on the page. One waits at a time. */
export const WAITING = { own: "Waiting for your approval", other: "Another request waits for your approval" };

/** An immediate order's fate, said in full: "less" alone leaves open
 *  whether it can be nothing and whether the rest keeps waiting. */
const ARRIVES = "Your order arrives in about a second.";
const NOTHING_WAITS = "Nothing is left waiting.";

/** The parts of a line with one space between a sentence and what follows
 *  it, and none at its end: a link never runs into the words around it. */
function spaced(parts: (string | Offer)[]): (string | Offer)[] {
  return parts.map((part, i) => (typeof part !== "string" ? part : i === parts.length - 1 ? part.trimEnd() : `${part.trimEnd()} `));
}

/** Said in both views when an order would meet one of the account's own. */
export const OWN_ORDER = "One of the orders this would trade with is your own. Trading with yourself changes nothing, and the seller's fee is still taken.";

export function advise(q: Asking): Advice {
  const { m, buy, pay, have, want, plan: p } = q;
  const verb = buy ? "Buy" : "Sell";
  const base = unit(m.base);
  // Buying costs more than selling brings at the same moment. The card
  // shows the price of the side it is on, and the other one beside it, so
  // nobody learns of the gap by buying and selling straight back.
  const across = q.other > 0n ? { note: `if you were ${buy ? "selling" : "buying"}: ${price(q.other, m)}` } : {};
  const now: Line[] = q.best > 0n ? [{ key: "Price now", value: priceText(q.best, m), ...across }] : [];
  const off = (go: string, ...hint: (string | Offer)[]): Advice => ({ go, enabled: false, lines: now, promise: "", hint: spaced(hint), warn: [] });
  // An offer changes the amount in the form. It is worded as that, so that
  // it cannot be taken for a second button that buys.
  const change = (lots: bigint): Offer => ({ link: `Change to ${size(lots, m)}`, act: "use", lots });
  if (q.blocked === "review") return off(WAITING.own);
  if (q.blocked === "elsewhere") return off(WAITING.other);
  if (q.blocked === "network") return off("Waiting for the network");
  if (q.blocked === "prices") return off("Waiting for live prices");
  if (q.best === 0n) {
    return off(buy ? `Nobody is selling ${base} right now` : `Nobody is buying ${base} right now`, "You can leave an order waiting at a price of your own instead. ", { link: "Open Pro", act: "pro" });
  }
  if (q.problem) return off("Enter an amount", q.problem);
  if (!want || !p) return off("Enter an amount");
  if (p.short === "small") {
    return off("That amount is too small", `The smallest amount is ${lotText(m)}, about ${amountText(quoteAtoms(1n, p.best, m), m.quote)} right now. `, { link: `Use ${lotText(m)}`, act: "use", lots: 1n });
  }
  if (p.short === "funds") {
    if (p.lots === 0n) return off(`Not enough ${unit(pay)}`, `You have ${amountText(have, pay)}. `, { link: "Get test funds", act: "funds" });
    // A budget beyond the balance can also be beyond the book: then the
    // amount offered is every lot for sale, and the line says so, or it
    // would look as if the balance bought no more than that.
    const all = buy && p.lots === p.offered;
    const enough = !buy ? "" : all ? `, and only ${size(p.offered, m)} is for sale right now` : `, enough for ${size(p.lots, m)}`;
    return off(`Not enough ${unit(pay)}`, `You have ${amountText(have, pay)}${enough}. `, change(p.lots));
  }
  if (p.short === "book") {
    return off(buy ? "Not that much is for sale" : "Not that much is wanted", buy ? `Only ${size(p.offered, m)} is for sale right now. ` : `Buyers want only ${size(p.offered, m)} right now. `, change(p.lots));
  }

  // It can go ahead.
  const sure = p.limit === p.best;
  const what = size(p.lots, m);
  const sum = money(p, buy, m);
  const pieces = `${base} is traded in pieces of ${lotText(m)}`;
  const atBest = quoteAtoms(p.lots, p.best, m);
  const apart = amountText(buy ? p.gross - atBest : atBest - p.gross, m.quote);
  const wide = !sure && reach(p) >= WIDE_BPS;
  const hint: (string | Offer)[] = [];
  // What the line under the card ends with: an offer, after its reason.
  const last: (string | Offer)[] = [];
  let lines: Line[];
  let promise: string;
  if (buy) {
    lines = [
      { key: "You pay", value: sum, ...(sure ? {} : { note: `never more than ${amountText(p.ceiling, m.quote)}` }) },
      { key: "Price", value: sure ? priceText(p.best, m) : `about ${averageText(p.gross, p.lots, m)}`, ...(sure ? across : {}) },
      { key: "Fee", value: "None for buying" },
    ];
    promise = `${ARRIVES} If sellers are gone by then, you get less or nothing and pay less, never more. ${NOTHING_WAITS}`;
    if (want.of === "quote" && want.atoms > p.ceiling) {
      // The budget did not come out even: say where the rest goes.
      const budget = amountText(want.atoms, m.quote);
      if (sure) {
        // At one price it is what a piece is too big for, to the unit, and
        // a piece more has a known cost.
        hint.push(`Your ${budget} buys ${what} for ${sum}. The other ${amountText(want.atoms - p.ceiling, m.quote)} stays yours, because ${pieces}.`);
        if (q.more) last.push(`One piece more would cost ${money(q.more, true, m)} in all.`, change(q.more.lots));
      } else {
        // Across several prices what is left is no fixed sum. Why it is
        // left says more than a number would: a piece more could pass the
        // budget. It is offered all the same, but not beside a warning
        // that says to take less.
        hint.push(`Your ${budget} buys ${what} for ${sum}.`);
        last.push(`One piece more could cost more than your ${budget}, so what is left of it stays yours.`);
        if (q.more && !wide) last.push(change(q.more.lots));
      }
    }
  } else {
    const worst = quoteAtoms(p.lots, p.limit, m);
    const floor = worst - sellerFee(worst, q.feePpm);
    lines = [
      { key: "You receive", value: sum, ...(sure ? {} : { note: `at least ${amountText(floor, m.quote)} for all of it` }) },
      { key: "Price", value: sure ? priceText(p.best, m) : `about ${averageText(p.gross, p.lots, m)}`, ...(sure ? across : {}) },
      { key: "Fee", value: p.fee > 0n ? `${amountText(p.fee, m.quote)}, already subtracted above` : "None" },
    ];
    promise = `${ARRIVES} If buyers are gone by then, you sell less or nothing, and never for less than ${priceText(p.limit, m)}. ${NOTHING_WAITS}`;
    if (want.of === "quote" && p.gross - p.fee !== want.atoms) hint.push(`That takes ${what}, which brings ${sum}.`);
  }
  if (want.of === "base" && want.atoms !== baseAtoms(p.lots, m)) hint.push(`${pieces}, so this is for ${what}.`);
  const warn: (string | Offer)[] = [];
  if (q.own) warn.push(OWN_ORDER);
  if (wide) {
    // The warning names what it is about: how much there is at the best
    // price, how far the rest goes, what that comes to in money, and that
    // the sum above has it in already. Then the amount that avoids it,
    // with what that one costs or brings.
    const only = quoteAtoms(q.first, p.best, m);
    warn.push(
      buy
        ? `Few sellers are waiting. Only ${size(q.first, m)} is for sale at the best price, ${priceText(p.best, m)}. The rest costs up to ${priceText(p.limit, m)}, so this costs about ${apart} more than at the best price. That is already counted above.`
        : `Few buyers are waiting. Only ${size(q.first, m)} is wanted at the best price, ${priceText(p.best, m)}. The rest sells for as little as ${priceText(p.limit, m)}, so this brings about ${apart} less than at the best price. That is already counted above.`,
      buy ? `To pay the best price only, ${amountText(only, m.quote)} in all:` : `To sell at the best price only, for ${amountText(only - sellerFee(only, q.feePpm), m.quote)}:`,
      change(q.first),
    );
  } else if (!sure) {
    hint.push(
      buy
        ? `Only ${size(q.first, m)} is for sale at the best price, so the rest costs more: about ${apart} more in all.`
        : `Only ${size(q.first, m)} is wanted at the best price, so the rest sells for less: about ${apart} less in all.`,
    );
  }
  hint.push(...last);
  return { go: `${verb} ${what} for ${sum}`, enabled: true, lines, promise, hint: spaced(hint), warn: spaced(warn) };
}

// ---- the order form: what a limit order would do right now -----------------

/** What a limit order would do against the other side of the book as the
 *  live view shows it: wait, trade, or some of each. `levels` is the side
 *  it would trade with, best first. An estimate, like everything read from
 *  the live view. */
export type Outlook = {
  /** A whole sentence, e.g. "Right now: all of it waits in the order book." */
  text: string;
  /** The network would refuse the order as the book stands. */
  refused: boolean;
};

export function outlook(side: "buy" | "sell", tif: string, ticks: bigint, lots: bigint, levels: Level[], m: Market): Outlook {
  const buy = side === "buy";
  let now = 0n;
  let gross = 0n;
  let first = 0n;
  let last = 0n;
  for (const level of levels) {
    const at = BigInt(level.price_ticks);
    if (now === lots || (buy ? at > ticks : at < ticks)) break;
    const take = lots - now < BigInt(level.lots) ? lots - now : BigInt(level.lots);
    if (now === 0n) first = at;
    last = at;
    now += take;
    gross += quoteAtoms(take, at, m);
  }
  const per = unit(m.quote);
  const say = (text: string, refused = false): Outlook => ({ text: `Right now: ${text}.`, refused });
  if (now === 0n) return say(tif === "ioc" ? `nothing would trade, because nobody ${buy ? "sells at this price or lower" : "buys at this price or higher"}` : "all of it waits in the order book");
  if (tif === "post_only") {
    return say(`it would be refused, because it could trade at once. Post only needs a price ${buy ? "below" : "above"} ${price(BigInt(levels[0].price_ticks), m)} ${per}`, true);
  }
  const at = first === last ? `at ${price(first, m)} ${per}` : `at about ${averagePrice(gross, now, m)} ${per}`;
  // What that part comes to: the estimate an order with a price of its
  // own otherwise never shows.
  const sum = `for ${amountText(gross, m.quote)}${buy ? "" : " before the fee"}`;
  if (now === lots) return say(`all of it trades at once, ${at}, ${sum}`);
  return say(`${size(now, m)} trades at once, ${at}, ${sum}, and the other ${size(lots - now, m)} ${tif === "ioc" ? "is dropped" : "waits"}`);
}

// ---- the approval window: what it asks before anything is signed -----------

/** A drafted action, as the core hands it to the approval window. */
export type Drafted = { chain: string; domain: string; account: string; intent: Intent; window_slots: number; seats: number; fee_ppm: number };

/** One fact about a request: a word under it where the fact needs one, and
 *  its untruncated form where the value is a shortened id. */
export type Fact = { key: string; value: string; hint?: string; full?: string };

export type Question = {
  title: string;
  /** What happens and what cannot, in two or three plain sentences. */
  explain: string;
  facts: Fact[];
  /** Under Details: what is signed exactly, by whom and for whom. */
  more: Fact[];
  /** What the approving button says; empty when the request must not be
   *  approved, because this version cannot show what it is. */
  approve: string;
  /** What the other button says: the same back-out by the same name here
   *  and on the Trade page, named by what it leaves as it is. */
  reject: string;
};

/** What the approval window asks about a drafted action.
 *
 *  The window knows the order, not the book: every amount it states is what
 *  the order's own price limit guarantees ("at most", "at least"), never an
 *  estimate, and it says whether the action can be undone. */
export function question(a: Drafted): Question {
  const i = a.intent;
  const more: Fact[] = [
    { key: "Trading account", value: shortId(a.account, 10, 6), full: a.account },
    { key: "Valid for", value: `about ${a.window_slots} seconds`, hint: `${a.window_slots} slots of the network. If it is not confirmed by then, it never can be.` },
    { key: "When you approve", value: "B3 Hive signs it and sends it", hint: `With your trading account's key, to the network's ${a.seats} validators. The Trade page shows the result once the network confirms it.` },
    { key: "Network", value: a.chain, full: a.domain },
  ];
  if (i.action === "place") {
    const buy = i.side === "buy";
    const m = i.market;
    const ticks = BigInt(i.price_ticks);
    const quote = BigInt(i.quote.atoms);
    const fee = sellerFee(quote, a.fee_ppm);
    const what = size(BigInt(i.lots), m);
    const limit = priceText(ticks, m);
    const most = amountText(quote, m.quote);
    const least = amountText(quote - fee, m.quote);
    const held = buy ? most : what;
    const rate = `${feeRate(a.fee_ppm)} of what the sale brings, rounded up`;
    // A bound holds whatever the book does. Which way the true amount can
    // differ is said beside it, so that a button that said "about" and a
    // question that says "at most" or "at least" do not read as two prices.
    const sold: Fact[] = buy
      ? [
          {
            key: "You pay at most",
            value: most,
            hint: i.tif === "ioc" ? "The most it can cost, not an estimate. Less if sellers ask a lower price. Buyers pay no fee." : "Set aside while the order waits. Buyers pay no fee.",
          },
        ]
      : [
          {
            key: "For all of it you receive at least",
            value: least,
            hint: `${i.tif === "ioc" ? "The least it can bring, not an estimate. " : ""}After the seller's fee. More if buyers pay a higher price.`,
          },
          { key: "Seller's fee", value: `${amountText(fee, m.quote)} at this price`, hint: `${rate}.` },
        ];
    const others = buy ? "sellers" : "buyers";
    const exact: Fact[] = [
      { key: "Market", value: m.name, full: m.id },
      { key: "Exactly", value: `${whole(BigInt(i.lots))} lots at ${whole(ticks)} ticks`, hint: `As it is signed. One lot is ${lotText(m)}. One tick moves the price by ${tickText(m)}.` },
      { key: "Kind of order", value: tifText(i.tif) },
    ];
    const bound: Fact = { key: "Price limit", value: limit, hint: buy ? "It never pays a higher price than this." : "It never sells at a lower price than this." };
    if (i.tif === "ioc") {
      return {
        title: buy ? `Buy ${what} for at most ${most}?` : `Sell ${what} for at least ${least}?`,
        explain: buy
          ? `You get up to ${what} and pay at most ${most}. You pay less if sellers ask less than ${limit}. If some of them are gone when your order arrives, you get less and pay less, never more. A trade cannot be undone.`
          : `You sell up to ${what}. For all of it you receive at least ${least}, after the seller's fee, and more if buyers pay more than ${limit}. If some of them are gone when your order arrives, you sell less, and never for less than ${limit}. A trade cannot be undone.`,
        facts: [
          {
            key: buy ? "You buy up to" : "You sell up to",
            value: what,
            hint: buy ? "Less if sellers are gone when your order arrives. Then you also pay less." : "Less if buyers are gone when your order arrives.",
          },
          ...sold,
          bound,
          { key: "If not all of it can trade at once", value: buy ? "The rest is not bought, and its cost stays yours" : "The rest is not sold and stays yours" },
        ],
        more: [...exact, ...more],
        approve: `${buy ? "Buy" : "Sell"} ${what}`,
        reject: buy ? "Don't buy" : "Don't sell",
      };
    }
    const waits = i.tif === "gtc";
    return {
      title: `${ordered(i)}?`,
      explain: waits
        ? `Whatever can trade at once at this price or ${buy ? "lower" : "higher"}, trades. The rest waits in the order book until someone ${buy ? "sells" : "buys"} at your price or you cancel it. While it waits, ${held} is set aside from your balance. What has traded cannot be undone.`
        : `It only waits in the order book, until someone ${buy ? "sells" : "buys"} at your price or you cancel it. If it could trade at once, the network refuses it and nothing happens. While it waits, ${held} is set aside from your balance.`,
      facts: [
        { key: buy ? "You buy up to" : "You sell up to", value: what, hint: `Whenever ${others} accept your price. That can be all of it, part of it, or none.` },
        ...sold,
        bound,
        waits ? { key: "What cannot trade at once", value: "Waits in the order book until you cancel it" } : { key: "If it could trade at once", value: "The network refuses all of it" },
      ],
      more: [...exact, ...more],
      // An order that waits promises no trade: the button places it.
      approve: "Place the order",
      reject: "Don't place it",
    };
  }
  if (i.action === "cancel") {
    return {
      title: "Cancel this order?",
      explain: "The order stops waiting in the order book, and what it still sets aside becomes available again. That takes about a second: until the network confirms it, the order can still trade. Cancelling costs nothing.",
      facts: [i.placed?.action === "place" ? { key: "Order", value: ordered(i.placed), hint: "As it was placed. Part of it may have traded since." } : { key: "Order", value: shortId(i.order, 10, 6), full: i.order }],
      more: [{ key: "Order number", value: shortId(i.order, 10, 6), full: i.order }, ...more],
      approve: "Cancel the order",
      reject: "Keep the order",
    };
  }
  if (i.action === "cancel_all") {
    return {
      title: "Cancel all your orders?",
      explain: "Every order this account has waiting stops, in every market of this network, and what they still set aside becomes available again. That takes about a second: until the network confirms it, the orders can still trade. Cancelling costs nothing.",
      facts: [],
      more,
      approve: "Cancel all orders",
      reject: "Keep the orders",
    };
  }
  if (i.action === "transfer") {
    const what = amountText(BigInt(i.amount.atoms), i.amount.asset);
    return {
      title: `Send ${what}?`,
      explain: "It goes to another account on this test network, with no fee. It stays inside FlowMesh: nothing moves on the B3 chain. A transfer cannot be undone, so check the account number.",
      facts: [
        { key: "You send", value: what },
        { key: "To account", value: shortId(i.to, 10, 6), full: i.to },
      ],
      more,
      approve: `Send ${what}`,
      reject: "Don't send",
    };
  }
  // The core drafts only what this window can show; never approve blind.
  return { title: "This request cannot be shown", explain: "This version of B3 Hive does not know this kind of action. Reject it.", facts: [], more: [], approve: "", reject: "Reject" };
}
