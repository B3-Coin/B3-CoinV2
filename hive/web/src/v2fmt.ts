// Text for the FlowMesh V2 regtest API card (v2market.ts). Pure functions,
// so they run under node --test without a window.

// Mirrors src-tauri/src/v2api.rs.
export type V2Asset = { name: string; id: string };
export type V2Market = { name: string; id: string; base: string; quote: string; base_atoms_per_lot: string; quote_atoms_per_tick: string };
export type V2Info = {
  mode: string;
  seat: number;
  seats: number;
  quorum: number;
  height: number;
  head_hash: string;
  state_root: string;
  certified: boolean;
  pending_actions: number;
  mainnet_enabled: boolean;
  assets: V2Asset[];
  markets: V2Market[];
};
export type V2AssetTotals = { asset: string; deposits: string; available: string; reserved: string; pending: string; fees_held: string; total_liabilities: string };
export type V2Totals = { height: number; certified: boolean; conservation: boolean; assets: V2AssetTotals[] };
export type V2Balance = { asset: string; available: string; reserved: string };
export type V2Account = { account: string; found: boolean; height: number; certified: boolean; operator_verified: boolean; balances: V2Balance[]; next_nonce: string | null };
export type V2Event = { height: number; totals: V2Totals | null; error: string | null };

/** Data older than this is marked stale, as on the rest of the Trade page. */
export const STALE_MS = 3000;

const NBSP = " ";

/** Whole atom counts in groups of three; anything else is shown as given. */
export function atoms(value: string): string {
  if (!/^[0-9]+$/.test(value)) return value;
  return value.replace(/\B(?=(\d{3})+(?!\d))/g, NBSP);
}

/** An asset's name from the operator's list, or its shortened id. */
export function assetName(info: V2Info | null, id: string): string {
  const found = info?.assets.find((a) => a.id === id);
  return found ? found.name : `${id.slice(0, 8)}…${id.slice(-6)}`;
}

export function statusText(info: V2Info): string {
  const head = info.certified ? `certified height ${info.height}` : "genesis, no certified state yet";
  const pending = info.pending_actions === 1 ? "1 pending action" : `${info.pending_actions} pending actions`;
  return `${info.mode}, seat ${info.seat} of ${info.seats}, quorum ${info.quorum}: ${head}, ${pending}.`;
}

/** The live line: the newest certified height and how fresh it is. */
export function liveText(height: number | null, receivedAt: number | null, now: number, error: string | null): string {
  if (error) return `The certified-state stream failed and is retrying: ${error}`;
  if (height === null || receivedAt === null) return "Waiting for the certified-state stream.";
  const age = Math.max(0, now - receivedAt);
  const seconds = Math.floor(age / 1000);
  const stale = age > STALE_MS ? " Stale: no newer certified state has arrived." : "";
  return `Certified state ${height}, received ${seconds} s ago.${stale}`;
}

export function totalsNote(totals: V2Totals): string {
  const at = totals.certified ? `certified height ${totals.height}` : "the genesis state";
  const conservation = totals.conservation ? "The operator reports that every asset's liabilities match its deposits." : "The operator does not report conservation for these totals.";
  return `Totals at ${at}. ${conservation}`;
}

export function accountText(account: V2Account): string {
  if (!account.found) return `No such account at height ${account.height}.`;
  const nonce = account.next_nonce === null ? "every nonce is used" : `next nonce ${account.next_nonce}`;
  const proof = account.operator_verified ? "the operator checked its proof against the certified head" : "the operator has not certified it yet";
  return `Account at height ${account.height}: ${nonce}; ${proof}.`;
}

export function isHash(text: string): boolean {
  return /^[0-9a-f]{64}$/.test(text);
}
