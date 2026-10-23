// How often a validator can expect to make a block, from the Modern PoS V1
// eligibility rule (doc/design/b3-modern-pos-spec.md §3): in each round a
// validator is eligible with probability weight / total weight, so over many
// blocks its share of them is about that, and one of its blocks is expected
// every total / weight blocks. It is an approximation: the counting weights
// change as stakes come and go, and recovery rounds stretch block times.

/** Mainnet's modern block interval (block_interval_seconds). */
export const BLOCK_SECONDS = 60;

/** A wait this long or longer is called out as a small stake. */
export const LONG_WAIT_SECONDS = 30 * 86_400;

export type Outlook = {
  /** This validator's share of blocks, 0 to 1. */
  share: number;
  /** Blocks expected between two of its own, at least 1. */
  blocksBetween: number;
  /** The same in seconds at one block an interval. */
  secondsBetween: number;
};

/** `weight` out of `total`, both in whole B3 and `weight` included in
 *  `total`. Nothing can be said without weight or without a total. */
export function outlook(weight: number, total: number): Outlook | null {
  if (!Number.isFinite(weight) || !Number.isFinite(total) || weight <= 0 || total <= 0) return null;
  const w = Math.min(weight, total);
  const share = w / total;
  const blocksBetween = Math.max(1, Math.round(total / w));
  return { share, blocksBetween, secondsBetween: blocksBetween * BLOCK_SECONDS };
}

/** The outlook once `amount` B3 more is staked: it joins this validator's
 *  weight and the network's. Weight counts in whole B3. */
export function stakeOutlook(amount: string, ownWeight: number, totalWeight: number): Outlook | null {
  const added = Math.floor(Number(amount.trim()));
  if (!Number.isFinite(added) || added < 0) return null;
  return outlook(ownWeight + added, totalWeight + added);
}

/** "1 in 2 400" style share, with the percentage when it is not tiny. */
export function shareText(o: Outlook): string {
  const pct = o.share * 100;
  const shown = pct >= 10 ? pct.toFixed(0) : pct >= 1 ? pct.toFixed(1) : pct >= 0.01 ? pct.toFixed(2) : "under 0.01";
  return `1 in ${group(o.blocksBetween)} (${shown}%)`;
}

/** A wait in the unit that reads best: "about 3 minutes", "about 2 hours",
 *  "about 40 days", "about 1.5 years". */
export function waitText(seconds: number): string {
  const units: [number, string][] = [
    [365.25 * 86_400, "year"],
    [86_400, "day"],
    [3_600, "hour"],
    [60, "minute"],
  ];
  for (const [size, name] of units) {
    const n = seconds / size;
    if (n >= 1 || name === "minute") {
      const shown = n >= 10 ? Math.round(n) : Math.round(n * 10) / 10;
      return `about ${group(shown)} ${name}${shown === 1 ? "" : "s"}`;
    }
  }
  return "about a minute";
}

function group(n: number): string {
  // Thin-space groups, as the amounts on the pages.
  const [whole, frac] = String(n).split(".");
  const grouped = whole.replace(/\B(?=(\d{3})+(?!\d))/g, " ");
  return frac ? `${grouped}.${frac}` : grouped;
}
