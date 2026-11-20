// What the Trade page says: the sentence a result becomes, and what the
// simple card's button and the line under it say in each state. Pure
// functions over the core's view models and the plan of fm2fmt.ts, with no
// page and no clock in them, so every sentence can be tested.
import {
  amountText,
  baseAtoms,
  decimals,
  lotText,
  outcomeText,
  price,
  priceText,
  quoteAtoms,
  refusal,
  shortId,
  unit,
  type Asset,
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
      return {
        title: all ? "All orders cancelled" : "Order cancelled",
        meta: what,
        detail: `${what ? `${what}. ` : ""}What ${all ? "they" : "it"} held is available again.`,
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

// ---- the simple card: what its button says, and the line under it ----------

/** Something the line under the card offers to do in one click. */
export type Offer = { link: string; act: "use"; lots: bigint } | { link: string; act: "funds" | "pro" };

export type Advice = {
  /** What the button says: exactly what it will do, or why it cannot. */
  go: string;
  enabled: boolean;
  /** The line under the card: words, and what it offers to do instead. */
  hint: (string | Offer)[];
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
  /** Whether anyone waits on the other side of the book. */
  offered: boolean;
  /** What stops every trade right now: a request already waiting for
   *  approval, too few validators, or no live prices. */
  blocked: "" | "review" | "network" | "prices";
};

/** The simple card's button and hint.
 *
 *  A plan for less than was asked is never sent as it is: the button stays
 *  off, says what is in the way, and the hint offers the amount that works,
 *  for the user to take or leave. */
export function advise(q: Asking): Advice {
  const { m, buy, pay, have, want, plan: p } = q;
  const verb = buy ? "Buy" : "Sell";
  const base = unit(m.base);
  const off = (go: string, ...hint: (string | Offer)[]): Advice => ({ go, enabled: false, hint });
  if (q.blocked === "review") return off("Waiting for your approval");
  if (q.blocked === "network") return off("Waiting for the network");
  if (q.blocked === "prices") return off("Waiting for live prices");
  if (!q.offered) {
    return off(buy ? `Nobody is selling ${base} right now` : `Nobody is buying ${base} right now`, "You can leave an order waiting at a price of your own instead. ", { link: "Open Pro", act: "pro" });
  }
  if (q.problem) return off("Enter an amount", q.problem);
  if (!want || !p) return off("Enter an amount");
  if (p.short === "small") {
    return off("That amount is too small", `The smallest amount is ${lotText(m)}, about ${amountText(quoteAtoms(1n, p.best, m), m.quote)} right now. `, { link: `Use ${lotText(m)}`, act: "use", lots: 1n });
  }
  if (p.short === "funds") {
    // A budget beyond the balance can also be beyond the book: then the
    // amount offered is every lot for sale, and the line says so, or it
    // would look as if the balance bought no more than that.
    const all = buy && p.lots > 0n && p.lots === p.offered;
    return off(
      `Not enough ${unit(pay)}`,
      all ? `You have ${amountText(have, pay)}, and only ${size(p.offered, m)} is for sale right now. ` : `You have ${amountText(have, pay)}. `,
      p.lots > 0n ? { link: `${verb} ${size(p.lots, m)} instead`, act: "use", lots: p.lots } : { link: "Add funds", act: "funds" },
    );
  }
  if (p.short === "book") {
    return off(buy ? "Not that much is for sale" : "Not that much is wanted", buy ? `Only ${size(p.offered, m)} is for sale right now. ` : `Buyers want only ${size(p.offered, m)} right now. `, {
      link: `${verb} ${size(p.lots, m)} instead`,
      act: "use",
      lots: p.lots,
    });
  }
  // It can go ahead. The hint says where the order differs from what was
  // typed, so the button's amount is never a surprise.
  const hint: string[] = [];
  const net = p.gross - p.fee;
  if (want.of === "base" && want.atoms !== baseAtoms(p.lots, m)) {
    hint.push(`${base} trades in steps of ${lotText(m)}, so this is for ${size(p.lots, m)}.`);
  } else if (buy && want.of === "quote" && want.atoms > p.ceiling) {
    hint.push(`It uses at most ${amountText(p.ceiling, m.quote)} of that, because ${base} trades in steps of ${lotText(m)}. The rest stays yours.`);
  } else if (!buy && want.of === "quote" && net !== want.atoms) {
    hint.push(`That takes ${size(p.lots, m)}, which brings about ${amountText(net, m.quote)}.`);
  }
  return { go: `${verb} ${size(p.lots, m)}`, enabled: true, hint };
}
