// Exact number formatting for the Trade page. The core sends FlowMesh
// amounts as integer strings: prices in B3 atoms per base unit, quantities
// in base units, B3 in atoms. Everything here is BigInt; no value the user
// reads or signs ever passes through floating point. This module imports
// nothing, so `node --test` can run its tests without a bundler.

export const B3_DECIMALS = 9;

const GROUP = " "; // narrow no-break space, as in shared/ui.ts

function pow10(n: number): bigint {
  return 10n ** BigInt(n);
}

/** v / 10^scale written out exactly, with at least `places` decimals (more
 *  only when the value needs them, so nothing is ever cut off). */
export function decimal(v: bigint, scale: number, places: number): string {
  const neg = v < 0n;
  const abs = neg ? -v : v;
  const int = abs / pow10(scale);
  let frac = scale > 0 ? (abs % pow10(scale)).toString().padStart(scale, "0").replace(/0+$/, "") : "";
  if (frac.length < places) frac = frac.padEnd(places, "0");
  return `${neg ? "-" : ""}${int}${frac ? "." + frac : ""}`;
}

/** Decimals needed to write v / 10^scale exactly. */
export function neededPlaces(v: bigint, scale: number): number {
  let abs = v < 0n ? -v : v;
  if (abs === 0n) return 0;
  let places = scale;
  while (places > 0 && abs % 10n === 0n) {
    abs /= 10n;
    places--;
  }
  return places;
}

/** Thousands grouping of the integer part only. */
export function group(text: string): string {
  const [int, frac] = text.split(".");
  const grouped = int.replace(/\B(?=(\d{3})+(?!\d))/g, GROUP);
  return frac === undefined ? grouped : `${grouped}.${frac}`;
}

/** What the Trade page needs to know about a market's units. */
export type Units = {
  /** The base asset's verified display decimals. */
  decimals: number;
  /** Price input step, B3 atoms per base unit. */
  tick: bigint;
  /** Quantity input step, base units. */
  lot: bigint;
};

/** A canonical price P (B3 atoms per base unit) is P x 10^(d-9) B3 per
 *  token: returned as an integer and the power of ten it is divided by. */
function scaledPrice(p: bigint, decimals: number): [bigint, number] {
  return decimals >= B3_DECIMALS ? [p * pow10(decimals - B3_DECIMALS), 0] : [p, B3_DECIMALS - decimals];
}

/** The price column's decimals: the tick's own, so every on-grid price
 *  shows exactly and all rows line up. */
export function pricePlaces(units: Units): number {
  const [v, scale] = scaledPrice(units.tick, units.decimals);
  return neededPlaces(v, scale);
}

/** A price in B3 per token, exact. */
export function formatPrice(p: bigint, units: Units): string {
  const [v, scale] = scaledPrice(p, units.decimals);
  return group(decimal(v, scale, pricePlaces(units)));
}

/** The decimals a set of token quantities needs, for one column. */
export function quantityPlaces(values: bigint[], decimals: number): number {
  return Math.max(0, ...values.map((v) => neededPlaces(v, decimals)));
}

/** A token quantity from base units, with `places` decimals. */
export function formatQuantity(q: bigint, decimals: number, places = 0): string {
  return group(decimal(q, decimals, places));
}

/** B3 from atoms, as the rest of the app shows it (at least 2 decimals). */
export function formatB3(atoms: bigint): string {
  return group(decimal(atoms, B3_DECIMALS, 2));
}

/** The B3 a quantity costs at a price: q x P atoms. */
export function notional(q: bigint, p: bigint): bigint {
  return q * p;
}

/** Spread of a bid and an ask: their difference as a price, and its share
 *  200 x (ask - bid) / (ask + bid) in percent, rounded half up to 2
 *  decimals. Null when the book is crossed or locked (bid >= ask). */
export function spread(bid: bigint, ask: bigint, units: Units): { difference: string; percent: string } | null {
  if (bid >= ask) return null;
  const hundredths = (200n * 100n * (ask - bid) * 2n + (ask + bid)) / (2n * (ask + bid));
  return { difference: formatPrice(ask - bid, units), percent: decimal(hundredths, 2, 2) };
}
