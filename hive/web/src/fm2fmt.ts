// Numbers of a FlowMesh V2 market, for the Trade page and the approval
// window. Everything is exact integer arithmetic on BigInt: amounts are
// atoms, prices are ticks and sizes are lots, and none of them goes through
// a floating-point number on its way to the screen.
//
// A market trades whole lots of its base asset at whole ticks of its quote
// asset: one lot is `base_atoms_per_lot` atoms of the base, and a price of
// p ticks means p x `quote_atoms_per_tick` atoms of the quote for one lot.
// Native B3 is shown in B3 (its atoms are the chain's base units, 10^9 to
// one B3). The genesis document names no decimals for any other asset, so
// those are shown in atoms and said to be.

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

const B3_ATOMS = 1_000_000_000n;
const GROUP = "\u202f"; // narrow no-break space, as the other pages group digits

function groupInt(digits: string): string {
  return digits.replace(/\B(?=(\d{3})+(?!\d))/g, GROUP);
}

/** A whole number with its digits grouped: "1 234 567". */
export function whole(n: bigint): string {
  return `${n < 0n ? "−" : ""}${groupInt((n < 0n ? -n : n).toString())}`;
}

/** Atoms of native B3 as B3: "1 234.5", with at least `min` decimals. */
export function b3(atoms: bigint, min = 0): string {
  const abs = atoms < 0n ? -atoms : atoms;
  let frac = (abs % B3_ATOMS).toString().padStart(9, "0").replace(/0+$/, "");
  while (frac.length < min) frac += "0";
  return `${atoms < 0n ? "−" : ""}${groupInt((abs / B3_ATOMS).toString())}${frac ? "." + frac : ""}`;
}

/** An amount without its unit: B3 for the native asset, atoms otherwise. */
export function amount(atoms: bigint, asset: Asset): string {
  return asset.native ? b3(atoms) : whole(atoms);
}

/** The unit `amount` is in: "B3", or "rUSD atoms". */
export function unit(asset: Asset): string {
  return asset.native ? "B3" : `${asset.name} atoms`;
}

/** "1.5 B3" or "400 rUSD atoms". */
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

/** An amount as typed, in the unit `amount` shows: B3 with up to nine
 *  decimals for the native asset, whole atoms otherwise. */
export function parseAmount(text: string, asset: Asset): bigint | null {
  if (!asset.native) return parseWhole(text);
  const plain = text.replace(/\s/g, "");
  const match = /^(\d{1,10})(?:\.(\d{0,9}))?$/.exec(plain);
  if (!match) return null;
  return BigInt(match[1]) * B3_ATOMS + BigInt((match[2] ?? "").padEnd(9, "0"));
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

/** A price in the quote asset's display unit, for one lot. */
export function price(ticks: bigint, m: Market): string {
  return amount(ticks * tick(m), m.quote);
}

/** A typed price for one lot, as ticks; null unless it is a whole number of
 *  ticks above zero. */
export function parsePrice(text: string, m: Market): bigint | null {
  const atoms = parseAmount(text, m.quote);
  if (atoms === null || atoms <= 0n || atoms % tick(m) !== 0n) return null;
  return atoms / tick(m);
}

/** What one lot is, e.g. "0.001 B3" or "1 TOK atoms". */
export function lotText(m: Market): string {
  return amountText(lot(m), m.base);
}

/** What one tick is, in the quote's unit. */
export function tickText(m: Market): string {
  return amountText(tick(m), m.quote);
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

// The engine's outcome codes (src/flowmesh2/core/codes.h), in plain words.
const OUTCOMES: Record<string, string> = {
  DEPOSIT_CREDITED: "Deposit credited",
  RESTING: "Resting in the book",
  FILLED: "Filled",
  PARTIAL_RESTING: "Partly filled, the rest is resting",
  IOC_DONE: "Done; what did not fill at once was dropped",
  CANCELLED: "Cancelled",
  TRANSFERRED: "Transferred",
  REJ_INSUFFICIENT_AVAILABLE: "Rejected: not enough available balance",
  REJ_UNKNOWN_MARKET: "Rejected: unknown market",
  REJ_UNKNOWN_ORDER: "Rejected: no such order",
  REJ_NOT_OWN_ORDER: "Rejected: not this account's order",
  REJ_ORDER_CLOSED: "Rejected: the order was already filled or cancelled",
  REJ_WOULD_CROSS: "Rejected: a post-only order may not match at once",
  REJ_BOOK_FULL: "Rejected: the book is full",
  REJ_FILL_CAP: "Rejected: too many fills for one order",
  REJ_PRICE_BAND: "Rejected: the price is outside the allowed band",
  REJ_OPEN_ORDER_CAP: "Rejected: too many open orders",
  REJ_BAD_PARAMS: "Rejected: invalid values",
  REJ_UNKNOWN_ASSET: "Rejected: unknown asset",
  REJ_NO_RECIPIENT: "Rejected: the recipient account does not exist",
  REJ_RESIDUAL_CROSS_UNSUPPORTED: "Rejected: the unfilled rest would cross another order of the same slot",
};

/** An outcome code's registry name in plain words; the name itself when
 *  this version does not know it. */
export function outcomeText(name: string): string {
  return OUTCOMES[name] ?? name;
}

const TIF: Record<string, string> = {
  gtc: "Limit",
  ioc: "Immediate or cancel",
  post_only: "Post only",
};

export function tifText(tif: string): string {
  return TIF[tif] ?? tif;
}

/** First and last characters of an id; the whole id goes in a title. */
export function shortId(id: string, head = 8, tail = 6): string {
  return id.length <= head + tail + 1 ? id : `${id.slice(0, head)}…${id.slice(-tail)}`;
}
