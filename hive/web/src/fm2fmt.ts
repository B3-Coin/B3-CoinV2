// Numbers of a FlowMesh V2 market, for the Trade page and the approval
// window. Everything is exact integer arithmetic on BigInt: amounts are
// atoms, prices are ticks and sizes are lots, and none of them goes through
// a floating-point number on its way to the screen.
//
// The engine trades whole lots of a market's base asset at whole ticks of
// its quote asset: one lot is `base_atoms_per_lot` atoms of the base, and a
// price of p ticks means p x `quote_atoms_per_tick` atoms of the quote for
// one lot. People do not think in lots and ticks, so the pages show amounts
// in the asset's own unit and a price for one whole unit of the base, and
// keep lots and ticks as the steps those numbers move in.
//
// Native B3 has nine decimals (its atoms are the chain's base units). The
// genesis document gives no other asset any decimals, so those are counted
// in whole numbers of their smallest amount.

export type Asset = { name: string; id: string; native: boolean };

export type Market = {
  name: string;
  id: string;
  base: Asset;
  quote: Asset;
  base_atoms_per_lot: string;
  quote_atoms_per_tick: string;
};

export type Level = { price_ticks: string; lots: string; orders: number };

const GROUP = " "; // narrow no-break space, as the other pages group digits

function groupInt(digits: string): string {
  return digits.replace(/\B(?=(\d{3})+(?!\d))/g, GROUP);
}

/** A whole number with its digits grouped: "1 234 567". */
export function whole(n: bigint): string {
  return `${n < 0n ? "−" : ""}${groupInt((n < 0n ? -n : n).toString())}`;
}

/** The decimal places an asset's amounts have: nine for native B3, none for
 *  every other asset, since the genesis document names none. */
export function decimals(asset: Asset): number {
  return asset.native ? 9 : 0;
}

/** The atoms in one whole unit of an asset. */
function unitAtoms(asset: Asset): bigint {
  return 10n ** BigInt(decimals(asset));
}

function decimal(atoms: bigint, asset: Asset, min: number, grouped: boolean): string {
  const places = decimals(asset);
  const one = unitAtoms(asset);
  const abs = atoms < 0n ? -atoms : atoms;
  let frac = places ? (abs % one).toString().padStart(places, "0").replace(/0+$/, "") : "";
  while (frac.length < Math.min(min, places)) frac += "0";
  const int = (abs / one).toString();
  return `${atoms < 0n ? "−" : ""}${grouped ? groupInt(int) : int}${frac ? "." + frac : ""}`;
}

/** An amount without its unit, digits grouped: "1 234.5". */
export function amount(atoms: bigint, asset: Asset, min = 0): string {
  return decimal(atoms, asset, min, true);
}

/** An amount as an input holds it: no grouping, so it parses back as typed. */
export function plain(atoms: bigint, asset: Asset): string {
  return decimal(atoms, asset, 0, false);
}

/** The name amounts of an asset are counted in. */
export function unit(asset: Asset): string {
  return asset.native ? "B3" : asset.name;
}

/** "1.5 B3" or "400 rUSD". */
export function amountText(atoms: bigint, asset: Asset): string {
  return `${amount(atoms, asset)} ${unit(asset)}`;
}

/** A whole number as typed: digits, with spaces between groups allowed.
 *  Null for anything else, and for more digits than a 64-bit value has. */
export function parseWhole(text: string): bigint | null {
  const digits = text.replace(/\s/g, "");
  if (!/^\d{1,19}$/.test(digits)) return null;
  return BigInt(digits);
}

/** An amount as typed, in the asset's unit: digits, spaces between groups,
 *  and a dot before at most as many decimals as the asset has. Null for
 *  anything else; nothing is rounded. */
export function parseAmount(text: string, asset: Asset): bigint | null {
  const places = decimals(asset);
  if (places === 0) return parseWhole(text);
  const match = new RegExp(`^(\\d{1,10})(?:\\.(\\d{0,${places}}))?$`).exec(text.replace(/\s/g, ""));
  if (!match) return null;
  return BigInt(match[1]) * unitAtoms(asset) + BigInt((match[2] ?? "").padEnd(places, "0"));
}

const lot = (m: Market): bigint => BigInt(m.base_atoms_per_lot);
const tick = (m: Market): bigint => BigInt(m.quote_atoms_per_tick);

/** The base atoms of `lots`. */
export function baseAtoms(lots: bigint, m: Market): bigint {
  return lots * lot(m);
}

/** The quote atoms of `lots` at `ticks`: lots x price x tick. */
export function quoteAtoms(lots: bigint, ticks: bigint, m: Market): bigint {
  return lots * ticks * tick(m);
}

/** The whole lots in `atoms` of the base; what is left over is not traded. */
export function lotsIn(atoms: bigint, m: Market): bigint {
  return atoms > 0n ? atoms / lot(m) : 0n;
}

// ---- prices ----------------------------------------------------------------

/** What a shown price is for. One whole unit of the base whenever one tick
 *  then comes to a whole number of quote atoms; one lot otherwise, so that
 *  every price on the screen is exact. */
type Basis = { atoms: bigint; step: bigint; unit: boolean };

function basis(m: Market): Basis {
  const one = unitAtoms(m.base);
  if ((tick(m) * one) % lot(m) === 0n) return { atoms: one, step: (tick(m) * one) / lot(m), unit: true };
  return { atoms: lot(m), step: tick(m), unit: false };
}

/** A price in the quote's unit, for what `priceFor` names. */
export function price(ticks: bigint, m: Market): string {
  return amount(ticks * basis(m).step, m.quote);
}

/** A price as an input holds it. */
export function plainPrice(ticks: bigint, m: Market): string {
  return plain(ticks * basis(m).step, m.quote);
}

/** What prices are for: "B3", or "0.0003 B3" when a whole unit has no exact
 *  price. */
export function priceFor(m: Market): string {
  const b = basis(m);
  return b.unit ? unit(m.base) : amountText(b.atoms, m.base);
}

/** "rUSD per B3". */
export function priceUnit(m: Market): string {
  return `${unit(m.quote)} per ${priceFor(m)}`;
}

/** "101 000 rUSD per B3". */
export function priceText(ticks: bigint, m: Market): string {
  return `${price(ticks, m)} ${priceUnit(m)}`;
}

/** A typed price as ticks; null unless it is a whole number of ticks above
 *  zero. */
export function parsePrice(text: string, m: Market): bigint | null {
  const atoms = parseAmount(text, m.quote);
  const step = basis(m).step;
  if (atoms === null || atoms <= 0n || atoms % step !== 0n) return null;
  return atoms / step;
}

/** A typed price as the nearest whole tick, one at least; null when it is
 *  not a number. */
export function nearestPrice(text: string, m: Market): bigint | null {
  const atoms = parseAmount(text, m.quote);
  if (atoms === null) return null;
  const step = basis(m).step;
  const ticks = (atoms + step / 2n) / step;
  return ticks > 0n ? ticks : 1n;
}

/** The step amounts move in, e.g. "0.001 B3". */
export function lotText(m: Market): string {
  return amountText(lot(m), m.base);
}

/** The step prices move in, e.g. "1 000 rUSD". */
export function tickText(m: Market): string {
  return amountText(basis(m).step, m.quote);
}

/** The most lots `available` atoms pay for: of the quote at `ticks` for a
 *  buy, of the base for a sell. */
export function maxLots(side: "buy" | "sell", available: bigint, ticks: bigint, m: Market): bigint {
  if (available <= 0n) return 0n;
  if (side === "sell") return available / lot(m);
  return ticks > 0n ? available / (ticks * tick(m)) : 0n;
}

/** The seller's fee on `quote` atoms when it is the seller's whole order:
 *  ceil(quote x ppm / 1 000 000), as the engine charges it over an order. */
export function sellerFee(quote: bigint, ppm: number): bigint {
  return (quote * BigInt(ppm) + 999_999n) / 1_000_000n;
}

/** A fee rate in parts per million as a percentage: 100 -> "0.01%". */
export function feeRate(ppm: number): string {
  const text = (BigInt(ppm) * 100n).toString().padStart(7, "0");
  const frac = text.slice(-6).replace(/0+$/, "");
  return `${BigInt(text.slice(0, -6))}${frac ? "." + frac : ""}%`;
}

export type Depth = { ticks: bigint; lots: bigint; total: bigint; orders: number };

/** A ladder, best first, with the running total of lots. */
export function depth(levels: Level[]): Depth[] {
  let total = 0n;
  return levels.map((level) => {
    const lots = BigInt(level.lots);
    total += lots;
    return { ticks: BigInt(level.price_ticks), lots, total, orders: level.orders };
  });
}

/** The gap between the best bid and the best ask, and its share of the ask
 *  in hundredths of a percent; null when a side is empty or they cross. */
export function spread(bids: Level[], asks: Level[]): { ticks: bigint; bps: bigint } | null {
  if (!bids.length || !asks.length) return null;
  const bid = BigInt(bids[0].price_ticks);
  const ask = BigInt(asks[0].price_ticks);
  if (ask <= bid) return null;
  return { ticks: ask - bid, bps: ((ask - bid) * 10_000n) / ask };
}

/** Hundredths of a percent as a percentage: 123 -> "1.23%". */
export function percent(bps: bigint): string {
  return `${bps / 100n}.${(bps % 100n).toString().padStart(2, "0")}%`;
}

// ---- trading at once -------------------------------------------------------
//
// "Buy now" and "sell now" are an immediate-or-cancel order with a limit the
// page works out from the book: the price of the last level the amount
// reaches. The book is one seat's unproved report, so everything read from
// it is an estimate. The limit is what holds: a buy sets aside lots x limit
// and can never cost more, a sell never goes below its limit, and whatever
// cannot trade at once is dropped.

/** What the user asked for: this much of the base, or this much of the
 *  quote (to spend at most when buying, to receive at least when selling). */
export type Want = { of: "base" | "quote"; atoms: bigint };

export type Plan = {
  /** The lots to order; zero when there is nothing to order. */
  lots: bigint;
  /** The order's limit in ticks: the last level the lots reach. */
  limit: bigint;
  /** The best price on offer, in ticks; zero for an empty side. */
  best: bigint;
  /** Quote atoms the reached levels come to at their own prices. */
  gross: bigint;
  /** The most a buy can cost: lots x limit, what the engine sets aside.
   *  Zero for a sell. */
  ceiling: bigint;
  /** The seller's fee on `gross`; zero for a buy. */
  fee: bigint;
  /** Why the plan is for less than was asked: the book offers no more, the
   *  balance pays for no more, or the amount is below one step. */
  short: null | "book" | "funds" | "small";
  /** The lots on offer on this side of the book. */
  offered: bigint;
};

/** The most lots a buy can ask for with `budget` quote atoms, when all of
 *  them are set aside at the price of the last level they reach. */
function lotsWithin(ladder: Depth[], budget: bigint, m: Market): bigint {
  let lots = 0n;
  for (const level of ladder) {
    const before = level.total - level.lots;
    const most = budget / (level.ticks * tick(m)) - before;
    if (most <= 0n) break;
    if (most < level.lots) return before + most;
    lots = level.total;
  }
  return lots;
}

/** What `lots` come to down the ladder, and the last price they reach. */
function walk(ladder: Depth[], lots: bigint, m: Market): { gross: bigint; limit: bigint } {
  let left = lots;
  let gross = 0n;
  let limit = 0n;
  for (const level of ladder) {
    if (left === 0n) break;
    const take = left < level.lots ? left : level.lots;
    gross += quoteAtoms(take, level.ticks, m);
    limit = level.ticks;
    left -= take;
  }
  return { gross, limit };
}

/** The fewest lots a sale needs to bring `want` quote atoms after the fee;
 *  every lot on offer when even that is not enough. */
function lotsForNet(ladder: Depth[], want: bigint, m: Market, feePpm: number): { lots: bigint; reached: boolean } {
  const net = (gross: bigint) => gross - sellerFee(gross, feePpm);
  let gross = 0n;
  for (const level of ladder) {
    const per = level.ticks * tick(m);
    if (net(gross + level.lots * per) >= want) {
      let low = 1n;
      let high = level.lots;
      while (low < high) {
        const mid = (low + high) / 2n;
        if (net(gross + mid * per) >= want) high = mid;
        else low = mid + 1n;
      }
      return { lots: level.total - level.lots + low, reached: true };
    }
    gross += level.lots * per;
  }
  return { lots: ladder.length ? ladder[ladder.length - 1].total : 0n, reached: false };
}

/** The immediate order that does what `want` asks, as far as the book and
 *  the balance allow. `levels` is the side it trades against, best first:
 *  the asks for a buy, the bids for a sell. `available` is the balance of
 *  the asset it pays with: the quote for a buy, the base for a sell. */
export function plan(side: "buy" | "sell", want: Want, levels: Level[], available: bigint, m: Market, feePpm: number): Plan {
  const ladder = depth(levels);
  const offered = ladder.length ? ladder[ladder.length - 1].total : 0n;
  const best = ladder.length ? ladder[0].ticks : 0n;
  let lots = 0n;
  let short: Plan["short"] = null;
  if (side === "buy") {
    const affordable = lotsWithin(ladder, available, m);
    if (want.of === "base") {
      lots = lotsIn(want.atoms, m);
      if (lots === 0n) short = "small";
      if (lots > offered) [lots, short] = [offered, "book"];
      if (lots > affordable) [lots, short] = [affordable, "funds"];
    } else {
      const budget = want.atoms < available ? want.atoms : available;
      lots = lotsWithin(ladder, budget, m);
      if (offered === 0n) short = "book";
      else if (want.atoms > available) short = "funds";
      else if (lots === 0n) short = "small";
      // Every lot on offer, with money left for at least one more.
      else if (lots === offered && budget / (ladder[ladder.length - 1].ticks * tick(m)) > offered) short = "book";
    }
  } else {
    const held = lotsIn(available, m);
    if (want.of === "base") {
      lots = lotsIn(want.atoms, m);
      if (lots === 0n) short = "small";
    } else {
      const needed = lotsForNet(ladder, want.atoms, m, feePpm);
      lots = needed.lots;
      if (!needed.reached) short = "book";
    }
    if (lots > offered) [lots, short] = [offered, "book"];
    if (lots > held) [lots, short] = [held, "funds"];
  }
  if (lots === 0n && short === null) short = offered === 0n ? "book" : "small";
  const { gross, limit } = walk(ladder, lots, m);
  return {
    lots,
    limit,
    best,
    gross,
    ceiling: side === "buy" ? quoteAtoms(lots, limit, m) : 0n,
    fee: side === "sell" ? sellerFee(gross, feePpm) : 0n,
    short,
    offered,
  };
}

/** How far the last price a plan reaches is from the best one, in
 *  hundredths of a percent of the best. */
export function reach(p: Plan): bigint {
  if (p.best === 0n || p.lots === 0n) return 0n;
  const gap = p.limit > p.best ? p.limit - p.best : p.best - p.limit;
  return (gap * 10_000n) / p.best;
}

// ---- words -----------------------------------------------------------------

// The engine's outcome codes (src/flowmesh2/core/codes.h), in plain words.
const OUTCOMES: Record<string, string> = {
  DEPOSIT_CREDITED: "Deposit credited",
  RESTING: "Waiting in the book",
  FILLED: "Filled",
  PARTIAL_RESTING: "Partly filled, the rest is waiting",
  IOC_DONE: "Done; what did not trade at once was dropped",
  CANCELLED: "Cancelled",
  TRANSFERRED: "Sent",
  REJ_INSUFFICIENT_AVAILABLE: "Refused: not enough available balance",
  REJ_UNKNOWN_MARKET: "Refused: unknown market",
  REJ_UNKNOWN_ORDER: "Refused: no such order",
  REJ_NOT_OWN_ORDER: "Refused: not this account's order",
  REJ_ORDER_CLOSED: "Refused: the order was already filled or cancelled",
  REJ_WOULD_CROSS: "Refused: a post-only order may not trade at once",
  REJ_BOOK_FULL: "Refused: the book is full",
  REJ_FILL_CAP: "Stopped: too many trades for one order",
  REJ_PRICE_BAND: "Refused: the price is outside the allowed band",
  REJ_OPEN_ORDER_CAP: "Refused: too many open orders",
  REJ_BAD_PARAMS: "Refused: invalid values",
  REJ_UNKNOWN_ASSET: "Refused: unknown asset",
  REJ_NO_RECIPIENT: "Refused: the recipient account does not exist",
  REJ_RESIDUAL_CROSS_UNSUPPORTED: "Refused: the unfilled rest would cross another order of the same slot",
};

/** An outcome code's registry name as a short label; the name itself when
 *  this version does not know it. */
export function outcomeText(name: string): string {
  return OUTCOMES[name] ?? name;
}

// Why the network refused something, to finish the sentence "... because".
const REFUSALS: Record<string, string> = {
  REJ_INSUFFICIENT_AVAILABLE: "the account did not have enough available for it",
  REJ_UNKNOWN_MARKET: "the network does not know this market",
  REJ_UNKNOWN_ORDER: "the network has no such order",
  REJ_NOT_OWN_ORDER: "the order belongs to another account",
  REJ_ORDER_CLOSED: "the order was already filled or cancelled",
  REJ_WOULD_CROSS: "it could have traded at once, and a post-only order must wait in the book",
  REJ_BOOK_FULL: "this market's book is full",
  REJ_FILL_CAP: "it reached the most trades the network allows for one order",
  REJ_PRICE_BAND: "its price is too far from the market's reference price",
  REJ_OPEN_ORDER_CAP: "the account already has the most open orders allowed",
  REJ_BAD_PARAMS: "its values were not valid",
  REJ_UNKNOWN_ASSET: "the network does not know this asset",
  REJ_NO_RECIPIENT: "the account it was sent to does not exist on this network yet",
  REJ_RESIDUAL_CROSS_UNSUPPORTED: "what was left would have crossed another order placed at the same moment, which the network does not settle yet",
};

/** Why the network refused, as the end of a sentence. */
export function refusal(name: string): string {
  return REFUSALS[name] ?? `the network answered ${name}`;
}

const TIF: Record<string, string> = {
  gtc: "Waits until it is filled or cancelled",
  ioc: "Trades at once, the rest is dropped",
  post_only: "Only ever waits in the book (post only)",
};

/** How long an order stays, in words. */
export function tifText(tif: string): string {
  return TIF[tif] ?? tif;
}

/** First and last characters of an id; the whole id goes in a title. */
export function shortId(id: string, head = 8, tail = 6): string {
  return id.length <= head + tail + 1 ? id : `${id.slice(0, head)}…${id.slice(-tail)}`;
}
