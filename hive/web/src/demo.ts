// A demo market for the Trade page while FlowMesh has no market on mainnet:
// made-up numbers that move, so the page can be seen working. Everything
// here is invented and labelled so on the page; nothing can be sent from a
// demo market (trade.ts blocks every action while it is shown).
import type { Deposit, MarketSummary, MarketView, Tick, Trade, TradeRequest, Level } from "./trade";

export const DEMO_MARKET_ID = "demo";

/** The demo token: 8 decimals, so a price P (B3 atoms per base unit) shows
 *  as P / 10 B3 per token. A tick of 1 is 0.1 B3; a lot is 0.01 token. */
const DECIMALS = 8;
const TOKEN = 10n ** BigInt(DECIMALS);
const TICK = 1n;
const LOT = TOKEN / 100n;
const FEE_PER_MILLE = 1n;
const HISTORY = 72;
const BOOK_LEVELS = 10;
const STEP_MS = 1500;

/** A small deterministic generator, so the demo looks the same every time. */
function rng(seed: number): () => number {
  let s = seed >>> 0;
  return () => {
    s = (s + 0x6d2b79f5) >>> 0;
    let t = s;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

type State = { price: bigint; ticks: Tick[]; trades: Trade[]; next: number; random: () => number };

let state: State | null = null;

function clearing(s: State, sequence: number, observed_ms: number): void {
  const r = s.random();
  // A random walk pulled back toward 2.5 B3, in whole ticks: the further
  // away, the harder, so the chart wanders without drifting off.
  const away = Number(s.price - 25n);
  const pull = Math.max(-0.25, Math.min(0.25, -away * 0.05));
  const move = r < 0.3 + pull ? -1n : r > 0.7 + pull ? 1n : 0n;
  s.price = s.price + move * TICK;
  if (s.price < 5n) s.price = 5n;
  const cleared = s.random() < 0.8;
  const volume = cleared ? BigInt(Math.floor(5 + s.random() * 120)) * TOKEN : 0n;
  s.ticks.push({ sequence, kind: "auction", cleared, price: s.price.toString(), volume: volume.toString() });
  if (cleared) {
    const notional = volume * s.price;
    const mine = s.random() < 0.15;
    const own = mine ? (BigInt(Math.floor(1 + s.random() * 20)) * TOKEN).toString() : null;
    s.trades.unshift({
      sequence,
      price: s.price.toString(),
      quantity: volume.toString(),
      notional: notional.toString(),
      fee: ((notional * FEE_PER_MILLE) / 1000n).toString(),
      own_bought: own !== null && s.random() < 0.5 ? own : own !== null ? "0" : null,
      own_sold: own !== null ? (own === "0" ? own : "0") : null,
      observed_ms,
    });
    if (s.trades.length > 40) s.trades.length = 40;
  }
}

function fresh(): State {
  const s: State = { price: 25n, ticks: [], trades: [], next: 1, random: rng(0xb3b3) };
  const now = Date.now();
  for (let i = 0; i < HISTORY; i++) clearing(s, s.next++, now - (HISTORY - i) * STEP_MS);
  return s;
}

function levels(s: State, side: "bids" | "asks"): Level[] {
  const out: Level[] = [];
  let cumulative = 0n;
  for (let i = 1; i <= BOOK_LEVELS; i++) {
    const price = side === "bids" ? s.price - BigInt(i) * TICK : s.price + BigInt(i) * TICK;
    if (price <= 0n) break;
    // Deeper levels hold more, with some unevenness.
    const size = BigInt(Math.floor(8 + i * 6 + s.random() * 30)) * TOKEN;
    cumulative += size;
    const own = side === "bids" && i === 2 ? (50n * TOKEN).toString() : "0";
    out.push({
      low: price.toString(),
      high: price.toString(),
      size: size.toString(),
      cumulative: cumulative.toString(),
      curves: 1 + Math.floor(s.random() * 4),
      own,
    });
  }
  return out;
}

export function demoSummary(): MarketSummary {
  return {
    market_id: DEMO_MARKET_ID,
    base_asset_id: "demo-token",
    vault_id: "demo-vault",
    running: true,
    paused: false,
    halt: "none",
    error: "",
  };
}

/** The first view starts the demo over; each later call adds one auction. */
export function demoView(first: boolean): MarketView {
  if (first || !state) state = fresh();
  else clearing(state, state.next++, Date.now());
  const s = state;
  const last = s.ticks[s.ticks.length - 1];
  const bidPrice = s.price - 2n * TICK;
  const bidQuantity = 50n * TOKEN;
  const bidReserved = bidQuantity * bidPrice;
  return {
    market_id: DEMO_MARKET_ID,
    base_asset_id: "demo-token",
    unchanged: false,
    quantity_lot: LOT.toString(),
    price_tick: TICK.toString(),
    metadata: { decimals: DECIMALS, ticker: "DEMO", name: "Demo token", test_only: true },
    verification: {
      source: "demo",
      endpoint: "",
      certificate_verified: true,
      account_state_verified: true,
      history_endpoint_reported: false,
      event_gap: false,
    },
    status: {
      certified: true,
      head: `demo-${last.sequence}`,
      next_sequence: last.sequence + 1,
      anchor_height: 867_470,
      active_seats: 5,
      quorum_required: 4,
      running: true,
      paused: false,
      reconciling: false,
      pending_handoff: false,
      halt: "none",
      error: "",
      pending_actions: 0,
    },
    account: {
      account_id: "demo-account",
      next_sequence: 4,
      base_available: (300n * TOKEN).toString(),
      base_reserved: "0",
      b3_available: (1_250n * 10n ** 9n).toString(),
      b3_reserved: bidReserved.toString(),
      orders: [
        {
          side: "bid",
          status: "open",
          filled: "0",
          remaining: bidQuantity.toString(),
          reserved: bidReserved.toString(),
          points: [{ price: bidPrice.toString(), quantity: bidQuantity.toString() }],
        },
      ],
    },
    book: {
      available: true,
      reason: "",
      head: `demo-${last.sequence}`,
      best_bid: (s.price - TICK).toString(),
      best_ask: (s.price + TICK).toString(),
      bids: { available: true, more: true, levels: levels(s, "bids") },
      asks: { available: true, more: true, levels: levels(s, "asks") },
      standing: { cleared: true, price: s.price.toString(), volume: (37n * TOKEN).toString() },
    },
    trades: [...s.trades],
    ticks: [...s.ticks],
    ticks_gap: false,
    waited: true,
  };
}

export function demoRequests(): TradeRequest[] {
  const now = Date.now();
  const done = { status: "executed", text: "Certified", code: "", fill_quantity: null, fill_price: null };
  return [
    {
      action_id: "demo-request-3",
      kind: "order",
      side: "bid",
      points: [{ price: "23", quantity: (50n * TOKEN).toString() }],
      sequence: 3,
      submitted_ms: now - 4 * 60_000,
      state: "certified",
      stage: "certified",
      reason: "",
      certified: true,
      outcome: { ...done, text: "Standing in the book" },
      settled: true,
      pending: false,
      uncertain: false,
    },
    {
      action_id: "demo-request-2",
      kind: "order",
      side: "ask",
      points: [{ price: "27", quantity: (20n * TOKEN).toString() }],
      sequence: 2,
      submitted_ms: now - 25 * 60_000,
      state: "certified",
      stage: "outcome_verified",
      reason: "",
      certified: true,
      outcome: { ...done, text: "Filled", fill_quantity: (20n * TOKEN).toString(), fill_price: "27" },
      settled: true,
      pending: false,
      uncertain: false,
    },
    {
      action_id: "demo-request-1",
      kind: "deposit",
      side: null,
      points: [],
      sequence: 1,
      submitted_ms: now - 2 * 3_600_000,
      state: "certified",
      stage: "certified",
      reason: "",
      certified: true,
      outcome: { ...done, text: "1,000.00 B3 admitted" },
      settled: true,
      pending: false,
      uncertain: false,
    },
  ];
}

export function demoDeposits(): Deposit[] {
  const now = Math.floor(Date.now() / 1000);
  return [
    { txid: "demo".padEnd(64, "0"), vout: 0, confirmations: 2_140, time: now - 2 * 3_600, swept: true, asset: "b3", amount: (1_000n * 10n ** 9n).toString(), vault_id: "demo-vault" },
    { txid: "demo".padEnd(64, "1"), vout: 1, confirmations: 12, time: now - 900, swept: false, asset: "token", amount: (25n * TOKEN).toString(), vault_id: "demo-vault" },
  ];
}

export const DEMO_STEP_MS = STEP_MS;
