// The Trade page: a FlowMesh token/B3 market's verified book, clearings,
// trades and this wallet's trading account, kept live while the page is
// open. Rules from doc/flowmesh-trading-ui.md and doc/flowmesh-order-book.md:
// prices are certified clearings by microblock sequence, never invented
// times; the book shows the node's exact levels and is not recomputed here;
// unknown is never shown as zero; data older than 3 s is marked stale.
// Orders and cancellations are drafted by the core, which checks them again,
// and are signed only from the approval window: this page never signs.
import { invoke } from "@tauri-apps/api/core";

import { amountNode, el, isMasked, shorten } from "../shared/ui";
import {
  B3_DECIMALS,
  decimal,
  formatB3,
  formatPrice,
  formatQuantity,
  parsePrice,
  parseQuantity,
  quantityPlaces,
  spread,
  type Units,
} from "./tradefmt";

// Mirrors src-tauri/src/flowmesh.rs.
type MarketSummary = { market_id: string; base_asset_id: string; running: boolean; paused: boolean; halt: string; error: string };
type Metadata = { decimals: number | null; ticker: string; name: string; test_only: boolean };
type Verification = {
  source: string;
  endpoint: string;
  certificate_verified: boolean;
  account_state_verified: boolean;
  history_endpoint_reported: boolean;
  event_gap: boolean;
};
type Status = {
  certified: boolean;
  head: string;
  next_sequence: number;
  anchor_height: number;
  active_seats: number;
  quorum_required: number;
  running: boolean;
  paused: boolean;
  reconciling: boolean;
  pending_handoff: boolean;
  halt: string;
  error: string;
  pending_actions: number;
};
type Point = { price: string; quantity: string };
type Order = { side: "bid" | "ask"; status: string; filled: string; remaining: string; reserved: string; points: Point[] };
type Account = {
  account_id: string;
  next_sequence: number;
  base_available: string;
  base_reserved: string;
  b3_available: string;
  b3_reserved: string;
  orders: Order[];
};
type Level = { low: string; high: string; size: string; cumulative: string; curves: number; own: string };
type Side = { available: boolean; more: boolean; levels: Level[] };
type Clearing = { cleared: boolean; price: string; volume: string };
type Book = {
  available: boolean;
  reason: string;
  head: string;
  best_bid: string | null;
  best_ask: string | null;
  bids: Side | null;
  asks: Side | null;
  standing: Clearing | null;
};
type Tick = { sequence: number; kind: string; cleared: boolean; price: string; volume: string };
type Trade = {
  sequence: number;
  price: string;
  quantity: string;
  notional: string;
  fee: string;
  own_bought: string | null;
  own_sold: string | null;
  observed_ms: number | null;
};
type Outcome = { status: string | null; text: string; fill_quantity: string | null; fill_price: string | null };
type Receipt = { action_id: string; accepted: boolean; state: string; stage: string; reason: string; outcome: Outcome };
type TradeRequest = {
  action_id: string;
  kind: "order" | "cancel" | "other";
  side: "bid" | "ask" | null;
  points: Point[];
  sequence: number | null;
  submitted_ms: number;
  state: string;
  stage: string;
  reason: string;
  certified: boolean;
  outcome: Outcome;
  settled: boolean;
  pending: boolean;
  uncertain: boolean;
};
/** The approval window's decision on a draft (DraftResult in lib.rs). */
export type TradeResult =
  | { outcome: "traded"; id: string; receipt: Receipt }
  | { outcome: "rejected"; id: string }
  | { outcome: "failed"; id: string; error: string }
  | { outcome: string; id: string };
type MarketView = {
  market_id: string;
  base_asset_id: string;
  unchanged: boolean;
  quantity_lot: string;
  price_tick: string;
  metadata: Metadata | null;
  verification: Verification;
  status: Status;
  account: Account | null;
  book: Book | null;
  trades: Trade[] | null;
  ticks: Tick[];
  ticks_gap: boolean;
  waited: boolean;
};

/** New orders stop, and the book is marked stale, after this long without a
 *  verified read. */
const STALE_MS = 3000;
/** Pace of reads the node did not hold for a change. */
const PACE_MS = 500;
const RETRY_MS = 2000;
/** Book rows per side. */
const BOOK_ROWS = 10;
/** Clearing ticks kept for the chart. */
const MAX_TICKS = 1024;
/** How long a newly appearing book price is highlighted. */
const FLASH_MS = 800;
/** A ladder's most prices (FLOWMESH_LADDER_MAX_STEPS). */
const LADDER_STEPS = 4;
/** Requests are read again this often while one is pending, else this. */
const REQUESTS_PENDING_MS = 2000;
const REQUESTS_IDLE_MS = 15000;

// ---- state -------------------------------------------------------------------

let active = false;
/** Bumped by every market, wallet or page change: a reply from an older
 *  generation is dropped, so it never lands on another market's view. */
let generation = 0;
let markets: MarketSummary[] = [];
let marketId: string | null = null;

type Market = {
  metadata: Metadata | null;
  verification: Verification | null;
  status: Status | null;
  lot: string;
  tick: string;
  account: Account | null;
  /** The view had an account section: absent means none exists. */
  accountKnown: boolean;
  book: Book | null;
  trades: Trade[];
  ticks: Tick[];
  lastRead: number;
};
let market: Market = blank();

/** The order form's side: bid buys the token with B3, ask sells it. */
let side: "bid" | "ask" = "bid";
/** Drafts this page opened, until the approval window decides them. */
const drafts = new Map<string, "order" | "cancel">();
let requests: TradeRequest[] = [];
let requestsRead = 0;
let requestsLoading = false;

function blank(): Market {
  return {
    metadata: null,
    verification: null,
    status: null,
    lot: "1",
    tick: "1",
    account: null,
    accountKnown: false,
    book: null,
    trades: [],
    ticks: [],
    lastRead: 0,
  };
}

function units(): Units | null {
  const decimals = market.metadata?.decimals;
  if (decimals === null || decimals === undefined) return null;
  return { decimals, tick: BigInt(market.tick), lot: BigInt(market.lot) };
}

/** The token's ticker or name; an unnamed token is "Token", or "Token
 *  e36ce3…9256" where its id tells markets apart. */
function tokenLabel(m: Metadata | null, assetId = ""): string {
  return m?.ticker || m?.name || (assetId ? `Token ${shorten(assetId, 6, 4)}` : "Token");
}

function stale(): boolean {
  return Date.now() - market.lastRead > STALE_MS;
}

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

// ---- lifecycle -----------------------------------------------------------------

/** The page was opened. */
export function startTrade(): void {
  if (active) return;
  active = true;
  void loadMarkets();
}

/** The page was left: its reads stop. */
export function stopTrade(): void {
  active = false;
  generation++;
}

/** Another wallet is in use: its account replaces this one's. */
export function tradeWalletChanged(): void {
  if (!active) return;
  generation++;
  market = blank();
  requests = [];
  void loadMarkets();
}

async function loadMarkets(): Promise<void> {
  const mine = ++generation;
  showError("");
  try {
    const list = await invoke<MarketSummary[]>("trade_markets");
    if (mine !== generation) return;
    markets = list;
  } catch (e) {
    if (mine !== generation) return;
    showError(String(e));
    el("tr-head").hidden = el("tr-body").hidden = true;
    return;
  }
  el("tr-empty").hidden = markets.length > 0;
  el("tr-head").hidden = el("tr-body").hidden = markets.length === 0;
  if (!markets.length) return;
  if (!marketId || !markets.some((m) => m.market_id === marketId)) marketId = markets[0].market_id;
  renderPicker();
  void follow(marketId, mine);
}

function selectMarket(id: string): void {
  if (id === marketId) return;
  marketId = id;
  market = blank();
  requests = [];
  requestsRead = 0;
  clearForm();
  showResult("", "");
  render();
  const mine = ++generation;
  void follow(id, mine);
}

/** Reads the market until the generation changes: a first full read, then
 *  live reads that the node holds up to 1.5 s for a new head. A reply that
 *  did not wait is paced, so a node without live updates is read at most
 *  twice a second. */
async function follow(id: string, mine: number): Promise<void> {
  let knownHead: string | null = null;
  while (active && mine === generation) {
    // With a remote endpoint the views wait while a review is open or being
    // sent, so no display read can delay the order (doc/flowmesh-trading-ui.md).
    if (drafts.size && market.verification?.source === "remote_endpoint") {
      await sleep(250);
      continue;
    }
    try {
      const last = market.ticks[market.ticks.length - 1];
      const v: MarketView = await invoke<MarketView>("trade_market", {
        marketId: id,
        knownHead,
        watch: knownHead !== null,
        ticksAfter: last ? last.sequence : null,
      });
      if (mine !== generation) return;
      apply(v);
      showError("");
      knownHead = v.status.head || null;
      if (!v.waited) await sleep(PACE_MS);
    } catch (e) {
      if (mine !== generation) return;
      showError(String(e));
      await sleep(RETRY_MS);
    }
  }
}

function apply(v: MarketView): void {
  market.lastRead = Date.now();
  market.status = v.status;
  market.verification = v.verification;
  if (v.metadata) market.metadata = v.metadata;
  market.lot = v.quantity_lot;
  market.tick = v.price_tick;
  if (!v.unchanged) {
    // A full reply: an absent account means this wallet has none. A new
    // account sequence means a request was taken in.
    if (v.account?.next_sequence !== market.account?.next_sequence) void loadRequests();
    market.account = v.account;
    market.accountKnown = true;
    if (v.book) market.book = v.book;
    if (v.trades) market.trades = v.trades;
  }
  // A gap means ticks were dropped before these: start the chart again
  // rather than join two runs as if nothing were missing.
  if (v.ticks_gap) market.ticks = [];
  const known = new Set(market.ticks.map((t) => t.sequence));
  market.ticks.push(...v.ticks.filter((t) => !known.has(t.sequence)));
  if (market.ticks.length > MAX_TICKS) market.ticks.splice(0, market.ticks.length - MAX_TICKS);
  // A quiet read changes only the status: repaint nothing else.
  if (v.unchanged && !v.ticks.length) renderHead();
  else render();
}

// ---- rendering -----------------------------------------------------------------

function showError(text: string): void {
  const banner = el("tr-error");
  banner.textContent = text;
  banner.hidden = !text;
}

function renderPicker(): void {
  const pick = el<HTMLSelectElement>("tr-market");
  pick.replaceChildren(
    ...markets.map((m) => {
      const option = document.createElement("option");
      option.value = m.market_id;
      const meta = m.market_id === marketId ? market.metadata : null;
      option.textContent = `${tokenLabel(meta, m.base_asset_id)} / B3`;
      option.title = `Market ${m.market_id}\nToken ${m.base_asset_id}`;
      return option;
    }),
  );
  if (marketId) pick.value = marketId;
}

let pickerLabel = "";

function render(): void {
  // The picker is rebuilt only when the market's name arrives, so an open
  // list is not closed under the user.
  const label = tokenLabel(market.metadata);
  if (label !== pickerLabel) {
    pickerLabel = label;
    renderPicker();
  }
  renderHead();
  renderBook();
  renderChart();
  renderOrders();
  renderTrades();
  renderBalance();
  renderRequests();
  renderForm();
}

function stateBadge(): [string, string, string] {
  const s = market.status;
  if (!s) return ["badge-neutral", "Loading", ""];
  if (s.halt && s.halt !== "none") return ["badge-danger", "Halted", s.halt];
  if (!s.certified) return ["badge-neutral", "Not started", "No certified microblock yet"];
  if (s.paused) return ["badge-warning", "Paused", "Trading is paused"];
  if (s.reconciling) return ["badge-info", "Catching up", "Reconciling with the current B3 tip; wait before trading"];
  if (s.pending_handoff) return ["badge-info", "Handoff", "A validator handoff is pending"];
  if (stale()) return ["badge-warning", "Stale", "No verified update for over 3 seconds"];
  return ["badge-success", "Live", "Verified updates are arriving"];
}

function renderHead(): void {
  const [cls, text, title] = stateBadge();
  const badge = el("tr-state");
  badge.className = `badge ${cls}`;
  badge.textContent = text;
  badge.title = title;
  el("tr-test").hidden = !market.metadata?.test_only;
  const s = market.status;
  const u = units();
  const last = [...market.ticks].reverse().find((t) => t.cleared) ?? market.trades[0];
  el("tr-last").textContent = last && u ? formatPrice(BigInt(last.price), u) : "—";
  const standing = market.book?.standing;
  el("tr-next").textContent =
    standing && u ? (standing.cleared ? `${formatPrice(BigInt(standing.price), u)} for ${formatQuantity(BigInt(standing.volume), u.decimals)}` : "No match") : "—";
  el("tr-seq").textContent = s?.certified ? `#${s.next_sequence - 1}` : "—";
  el("tr-quorum").textContent = s ? `${s.active_seats} / ${s.quorum_required}` : "—";

  const v = market.verification;
  const parts: string[] = [];
  if (v) {
    parts.push(v.source === "local_engine" ? "This node's FlowMesh engine" : `Trading endpoint ${v.endpoint}`);
    parts.push(v.certificate_verified ? "certificate verified" : "certificate not verified");
    if (v.account_state_verified) parts.push("balances verified from state");
    if (v.history_endpoint_reported) parts.push("history reported by the endpoint");
    if (v.event_gap) parts.push("updates were missed at least once");
  }
  if (s?.pending_actions) parts.push(`${s.pending_actions} queued, not yet certified`);
  if (s?.error) parts.push(s.error);
  el("tr-source").textContent = parts.join(" · ");
  el("tr-book-stale").hidden = !market.status || !stale();
  renderGate();
}

// Book prices seen in the last render, per side, to flash new ones.
const shown = { asks: new Set<string>(), bids: new Set<string>() };

function renderBook(): void {
  const u = units();
  const book = market.book;
  const asks = el("tr-asks");
  const bids = el("tr-bids");
  const label = tokenLabel(market.metadata);
  el("tr-book-size").textContent = market.metadata ? `Size (${label})` : "Size";
  if (!book || !u) {
    asks.replaceChildren();
    bids.replaceChildren();
    el("tr-spread").textContent = !u && market.metadata ? "The token's decimals are not verified, so prices cannot be shown." : "—";
    return;
  }
  if (!book.available) {
    asks.replaceChildren();
    bids.replaceChildren();
    el("tr-spread").textContent = `Levels unavailable (${book.reason || "unknown"}); read again shortly.`;
    return;
  }
  const askLevels = (book.asks?.levels ?? []).slice(0, BOOK_ROWS);
  const bidLevels = (book.bids?.levels ?? []).slice(0, BOOK_ROWS);
  const all = [...askLevels, ...bidLevels];
  const places = quantityPlaces(
    all.flatMap((l) => [BigInt(l.size), BigInt(l.cumulative)]),
    u.decimals,
  );
  // One depth scale for both sides.
  const deepest = all.reduce((max, l) => (BigInt(l.cumulative) > max ? BigInt(l.cumulative) : max), 0n);
  const row = (level: Level, side: "asks" | "bids", previous: Set<string>) => {
    // A bid level is labelled by its lower edge, an ask by its upper: every
    // order in the level accepts that price.
    const price = side === "bids" ? level.low : level.high;
    const div = document.createElement("div");
    div.className = `tr-book-row tr-${side === "asks" ? "ask" : "bid"}`;
    if (BigInt(level.own) > 0n) div.classList.add("tr-own");
    if (previous.size && !previous.has(price)) {
      div.classList.add("tr-flash");
      setTimeout(() => div.classList.remove("tr-flash"), FLASH_MS);
    }
    const share = deepest > 0n ? Number((BigInt(level.cumulative) * 1000n) / deepest) / 10 : 0;
    div.style.setProperty("--depth", `${share}%`);
    const cell = (text: string, cls = "") => {
      const span = document.createElement("span");
      span.className = cls;
      span.textContent = text;
      return span;
    };
    div.append(
      cell(formatPrice(BigInt(price), u), "tr-price"),
      cell(formatQuantity(BigInt(level.size), u.decimals, places), "num"),
      cell(formatQuantity(BigInt(level.cumulative), u.decimals, places), "num"),
    );
    const own = BigInt(level.own) > 0n ? `\nYours: ${formatQuantity(BigInt(level.own), u.decimals, places)}` : "";
    const range = level.low === level.high ? "" : `\nPrices ${formatPrice(BigInt(level.low), u)} to ${formatPrice(BigInt(level.high), u)}`;
    div.title = `${level.curves} order${level.curves === 1 ? "" : "s"}${range}${own}\nClick to use this price`;
    div.classList.add("tr-pick");
    div.addEventListener("click", () => pickPrice(BigInt(price)));
    return div;
  };
  // Asks above the spread, best (lowest) nearest it.
  asks.replaceChildren(...[...askLevels].reverse().map((l) => row(l, "asks", shown.asks)));
  bids.replaceChildren(...bidLevels.map((l) => row(l, "bids", shown.bids)));
  shown.asks = new Set(askLevels.map((l) => l.high));
  shown.bids = new Set(bidLevels.map((l) => l.low));

  const spreadRow = el("tr-spread");
  spreadRow.title = "";
  if (book.best_bid !== null && book.best_ask !== null) {
    const s = spread(BigInt(book.best_bid), BigInt(book.best_ask), u);
    if (s) {
      spreadRow.textContent = `Spread ${s.difference} · ${s.percent}%`;
    } else {
      // Legal after a rationed auction; the next auction uncrosses it.
      const next = book.standing;
      spreadRow.textContent =
        next && next.cleared
          ? `Crossed · next auction ${formatPrice(BigInt(next.price), u)} for ${formatQuantity(BigInt(next.volume), u.decimals)} (indicative)`
          : "Crossed";
    }
  } else {
    spreadRow.textContent = askLevels.length || bidLevels.length ? "One-sided book" : "No standing orders";
  }
}

const SVG = "http://www.w3.org/2000/svg";

function svg(tag: string, attrs: Record<string, string>): SVGElement {
  const node = document.createElementNS(SVG, tag);
  for (const [k, v] of Object.entries(attrs)) node.setAttribute(k, v);
  return node;
}

function renderChart(): void {
  const chart = el("tr-chart");
  const u = units();
  const ticks = market.ticks;
  const cleared = ticks.filter((t) => t.cleared);
  el("tr-chart-empty").hidden = cleared.length > 0;
  if (!u || !cleared.length) {
    chart.replaceChildren();
    el("tr-chart-high").textContent = el("tr-chart-low").textContent = "";
    el("tr-chart-first").textContent = el("tr-chart-last").textContent = "";
    return;
  }
  const [width, height, pad] = [600, 200, 12];
  const first = ticks[0].sequence;
  const lastSeq = ticks[ticks.length - 1].sequence;
  const prices = cleared.map((t) => BigInt(t.price));
  const hi = prices.reduce((a, b) => (b > a ? b : a));
  const lo = prices.reduce((a, b) => (b < a ? b : a));
  // Plot positions only; every label is formatted exactly from the BigInt.
  const span = Number(hi - lo) || 1;
  const x = (seq: number) => (lastSeq === first ? width / 2 : ((seq - first) / (lastSeq - first)) * (width - 2 * pad) + pad);
  const y = (p: bigint) => (hi === lo ? height / 2 : height - pad - (Number(p - lo) / span) * (height - 2 * pad));
  const grid = [0.25, 0.5, 0.75].map((f) => svg("line", { class: "grid", x1: "0", x2: String(width), y1: String(height * f), y2: String(height * f) }));
  // A step line: the price holds until the next clearing.
  const points: string[] = [];
  cleared.forEach((t, i) => {
    const py = y(BigInt(t.price)).toFixed(1);
    if (i > 0) points.push(`${x(t.sequence).toFixed(1)},${y(BigInt(cleared[i - 1].price)).toFixed(1)}`);
    points.push(`${x(t.sequence).toFixed(1)},${py}`);
  });
  points.push(`${x(lastSeq).toFixed(1)},${y(BigInt(cleared[cleared.length - 1].price)).toFixed(1)}`);
  const line = svg("polyline", { class: "line", points: points.join(" ") });
  const dots = cleared.map((t) => {
    const dot = svg("circle", { class: "dot", cx: x(t.sequence).toFixed(1), cy: y(BigInt(t.price)).toFixed(1), r: "3" });
    const title = svg("title", {});
    title.textContent = `#${t.sequence}: ${formatPrice(BigInt(t.price), u)} B3 for ${formatQuantity(BigInt(t.volume), u.decimals)}`;
    dot.append(title);
    return dot;
  });
  chart.replaceChildren(...grid, line, ...dots);
  el("tr-chart-high").textContent = formatPrice(hi, u);
  el("tr-chart-low").textContent = hi === lo ? "" : formatPrice(lo, u);
  el("tr-chart-first").textContent = `#${first}`;
  el("tr-chart-last").textContent = `#${lastSeq}`;
}

/** The limit prices of a standing curve: its points that hold quantity at
 *  the edge it trades from. One price for a plain order, up to four for a
 *  ladder. */
function limits(order: Order): bigint[] {
  const points = order.points.map((p) => ({ price: BigInt(p.price), quantity: BigInt(p.quantity) }));
  const out: bigint[] = [];
  for (let i = 0; i < points.length; i++) {
    const next = points[i + 1];
    const prev = points[i - 1];
    // A bid's quantity drops after its limit; an ask's rises at its limit.
    if (order.side === "bid" && points[i].quantity > 0n && (!next || next.quantity < points[i].quantity)) out.push(points[i].price);
    if (order.side === "ask" && points[i].quantity > 0n && (!prev || prev.quantity < points[i].quantity)) out.push(points[i].price);
  }
  return order.side === "bid" ? out.reverse() : out;
}

function renderOrders(): void {
  const rows = el("tr-orders-rows");
  const note = el("tr-orders-note");
  const u = units();
  const account = market.account;
  el("tr-cancels").hidden = true;
  if (!market.accountKnown) {
    rows.replaceChildren();
    note.textContent = "Loading…";
    return;
  }
  if (!account || !u) {
    rows.replaceChildren();
    note.textContent = account ? "The token's decimals are not verified." : "No trading account in this market yet.";
    return;
  }
  const label = tokenLabel(market.metadata);
  rows.replaceChildren(
    ...account.orders.map((o) => {
      const tr = document.createElement("tr");
      const td = (content: string | Node, cls = "") => {
        const cell = document.createElement("td");
        if (cls) cell.className = cls;
        cell.append(content);
        tr.append(cell);
      };
      const side = document.createElement("span");
      side.className = `badge ${o.side === "bid" ? "badge-success" : "badge-danger"}`;
      side.textContent = o.side === "bid" ? "Buy" : "Sell";
      side.title = o.status === "partially-filled" ? "Open, partly filled" : "Open";
      td(side);
      // A ladder's prices stack, one per line.
      const prices = document.createElement("div");
      for (const p of limits(o)) {
        const line = document.createElement("div");
        line.textContent = formatPrice(p, u);
        prices.append(line);
      }
      td(prices.childElementCount ? prices : "—", "num");
      td(`${formatQuantity(BigInt(o.remaining), u.decimals)} ${label}`, "num");
      td(`${formatQuantity(BigInt(o.filled), u.decimals)} ${label}`, "num");
      td(o.side === "bid" ? `${formatB3(BigInt(o.reserved))} B3` : `${formatQuantity(BigInt(o.reserved), u.decimals)} ${label}`, "num");
      if (isMasked()) for (const cell of [...tr.children].slice(2, 5)) cell.textContent = "••••";
      return tr;
    }),
  );
  // One cancellation per side, under the table; renderGate enables them.
  for (const s of ["bid", "ask"] as const) el(`tr-cancel-${s}`).hidden = !openOrder(s);
  el("tr-cancels").hidden = !openOrder("bid") && !openOrder("ask");
  note.textContent = account.orders.length
    ? "One order per side: a new Buy replaces your Buy, a new Sell your Sell. Amounts are certified; a cancellation takes effect once certified."
    : "No open orders.";
}

function renderTrades(): void {
  const rows = el("tr-trades-rows");
  const u = units();
  el("tr-trades-empty").hidden = market.trades.length > 0;
  if (!u) {
    rows.replaceChildren();
    return;
  }
  const places = quantityPlaces(
    market.trades.map((t) => BigInt(t.quantity)),
    u.decimals,
  );
  rows.replaceChildren(
    ...market.trades.map((t) => {
      const tr = document.createElement("tr");
      const td = (text: string, cls = "num", title = "") => {
        const cell = document.createElement("td");
        cell.className = cls;
        cell.textContent = text;
        if (title) cell.title = title;
        tr.append(cell);
      };
      td(formatPrice(BigInt(t.price), u));
      td(formatQuantity(BigInt(t.quantity), u.decimals, places));
      td(formatB3(BigInt(t.notional)), "num", `Fee ${formatB3(BigInt(t.fee))} B3, paid by the seller`);
      td(t.observed_ms ? new Date(t.observed_ms).toLocaleTimeString() : "—", "tnum", "Operator-observed, not certified");
      let yours = "—";
      if (t.own_bought !== null && t.own_sold !== null) {
        const bought = BigInt(t.own_bought);
        const sold = BigInt(t.own_sold);
        yours = isMasked()
          ? bought || sold
            ? "••••"
            : ""
          : [bought ? `Bought ${formatQuantity(bought, u.decimals)}` : "", sold ? `Sold ${formatQuantity(sold, u.decimals)}` : ""].filter(Boolean).join(", ");
      }
      td(yours, "num nowrap", yours === "—" ? "Unknown: outside what the node keeps for your account" : "");
      td(`#${t.sequence}`);
      return tr;
    }),
  );
}

function renderBalance(): void {
  const u = units();
  const account = market.account;
  const label = tokenLabel(market.metadata);
  el("tr-base-free-label").textContent = `${label} available`;
  el("tr-base-held-label").textContent = `${label} in orders`;
  el("tr-no-account").hidden = !market.accountKnown || account !== null;
  el("tr-balance").hidden = market.accountKnown && account === null;
  const b3 = (id: string, atoms: string | undefined) =>
    el(id).replaceChildren(atoms === undefined ? "—" : amountNode(decimal(BigInt(atoms), B3_DECIMALS, 2)));
  b3("tr-b3-free", account?.b3_available);
  b3("tr-b3-held", account?.b3_reserved);
  const token = (id: string, units_: string | undefined) => {
    el(id).textContent = units_ === undefined || !u ? "—" : isMasked() ? "••••" : `${formatQuantity(BigInt(units_), u.decimals)} ${label}`;
  };
  token("tr-base-free", account?.base_available);
  token("tr-base-held", account?.base_reserved);
}


// ---- orders --------------------------------------------------------------------

type Step = { price: bigint; quantity: bigint };

const tokenWord = () => tokenLabel(market.metadata);

/** Why a new order or cancellation cannot be drafted now, or null. The core
 *  checks all of it again against fresh data. */
function gate(): string | null {
  const s = market.status;
  if (!s || !market.accountKnown) return "Loading the market…";
  if (!units()) return "The token's decimals are not verified, so orders cannot be checked.";
  if (s.halt && s.halt !== "none") return `This market is halted (${s.halt}).`;
  if (!s.certified) return "This market has no certified state yet.";
  if (s.paused) return "Trading in this market is paused.";
  if (s.reconciling) return "The node is catching up with the B3 chain.";
  if (!market.account) return "This wallet has no trading account in this market. The first deposit creates it.";
  if (stale()) return "New orders wait for a verified update: there has been none for over 3 seconds.";
  if (drafts.size) return "Waiting for your decision in the approval window.";
  if (requests.some((r) => r.pending)) return "Your previous request is not certified yet. New requests wait for it, so the two do not compete; see Requests.";
  return null;
}

function renderGate(): void {
  const reason = gate();
  el("tr-gate").textContent = reason ?? "";
  el("tr-gate").hidden = reason === null;
  el<HTMLButtonElement>("tr-review").disabled = reason !== null;
  el<HTMLButtonElement>("tr-cancel-bid").disabled = reason !== null;
  el<HTMLButtonElement>("tr-cancel-ask").disabled = reason !== null;
}

function stepCount(): number {
  return el<HTMLInputElement>("tr-ladder").checked ? LADDER_STEPS : 1;
}

/** The form's rows as exact steps, null while nothing is entered, or the
 *  first problem. Nothing is rounded: an off-grid value is an error. */
function readSteps(u: Units): Step[] | null | string {
  const steps: Step[] = [];
  const ladder = stepCount() > 1;
  for (let i = 0; i < stepCount(); i++) {
    const priceText = el<HTMLInputElement>(`tr-price-${i}`).value.trim();
    const quantityText = el<HTMLInputElement>(`tr-qty-${i}`).value.trim();
    const where = ladder ? `Price ${i + 1}: ` : "";
    if (!priceText && !quantityText) {
      if (i === 0) return null;
      continue;
    }
    if (!priceText || !quantityText) return `${where}enter both a price and a quantity.`;
    const price = parsePrice(priceText, u);
    if ("error" in price) return where + price.error;
    const quantity = parseQuantity(quantityText, u);
    if ("error" in quantity) return where + quantity.error;
    if (steps.some((s) => s.price === price.value)) return `${where}every price of a ladder must differ.`;
    steps.push({ price: price.value, quantity: quantity.value });
  }
  return steps;
}

function openOrder(s: "bid" | "ask"): Order | undefined {
  return market.account?.orders.find((o) => o.side === s && o.remaining !== "0");
}

/** What the entered order reserves and may reserve: the free balance plus
 *  what the order it replaces holds. */
function reservation(steps: Step[]): { reserve: bigint; available: bigint } {
  const a = market.account;
  const held = BigInt(openOrder(side)?.reserved ?? "0");
  if (side === "bid") {
    return { reserve: steps.reduce((sum, s) => sum + s.price * s.quantity, 0n), available: BigInt(a?.b3_available ?? "0") + held };
  }
  return { reserve: steps.reduce((sum, s) => sum + s.quantity, 0n), available: BigInt(a?.base_available ?? "0") + held };
}

function renderForm(): void {
  const u = units();
  const token = tokenWord();
  const buy = side === "bid";
  el("tr-buy").setAttribute("aria-selected", String(buy));
  el("tr-sell").setAttribute("aria-selected", String(!buy));
  el("tr-review").textContent = buy ? "Review buy order" : "Review sell order";
  document.querySelector("#tr-step-0 [data-tr-price]")!.textContent = `Price (B3 per ${token})`;
  document.querySelector("#tr-step-0 [data-tr-qty]")!.textContent = `Quantity (${token})`;
  for (let i = 1; i < LADDER_STEPS; i++) el(`tr-step-${i}`).hidden = stepCount() === 1;
  el("tr-steps-help").textContent = u
    ? `Steps: ${formatPrice(u.tick, u)} B3 per ${token} · ${formatQuantity(u.lot, u.decimals)} ${token}. ${buy ? "Buys at your price or below." : "Sells at your price or above."}`
    : "";
  const error = el("tr-form-error");
  const steps = u ? readSteps(u) : null;
  error.textContent = typeof steps === "string" ? steps : "";
  error.hidden = typeof steps !== "string";
  const summary = el("tr-summary");
  const replaced = openOrder(side);
  if (!u || steps === null || typeof steps === "string" || !market.account) {
    summary.hidden = true;
  } else {
    const { reserve, available } = reservation(steps);
    const amount = (v: bigint) => (buy ? `${formatB3(v)} B3` : `${formatQuantity(v, u.decimals)} ${token}`);
    summary.hidden = false;
    el("tr-reserve-label").textContent = buy ? "Reserves at most" : "Reserves";
    el("tr-reserve").textContent = amount(reserve);
    el("tr-available").textContent = isMasked() ? "••••" : amount(available);
    el("tr-replaces-row").hidden = !replaced;
    if (replaced) {
      el("tr-replaces").textContent = isMasked() ? "••••" : `your open ${buy ? "buy" : "sell"}, ${formatQuantity(BigInt(replaced.remaining), u.decimals)} ${token} left`;
    }
    if (reserve > available) {
      error.textContent = buy
        ? "This order costs more B3 than your trading balance has available."
        : `This order sells more ${token} than your trading balance has available.`;
      error.hidden = false;
    }
  }
  renderGate();
}

function clearForm(): void {
  for (let i = 0; i < LADDER_STEPS; i++) {
    el<HTMLInputElement>(`tr-price-${i}`).value = "";
    el<HTMLInputElement>(`tr-qty-${i}`).value = "";
  }
}

/** A price as typed into the form: exact, without grouping. */
function plainPrice(p: bigint, u: Units): string {
  return formatPrice(p, u).replace(/\u202f/g, "");
}

/** A book level's price into the form's first row. It is taken from the
 *  verified levels the page holds, never from the text, and refused while
 *  they are not current. */
function pickPrice(price: bigint): void {
  const u = units();
  if (!u) return;
  if (stale()) {
    showResult("warning", "The book is not current, so its price was not taken. Wait for a verified update.");
    return;
  }
  el<HTMLInputElement>("tr-price-0").value = plainPrice(price, u);
  renderForm();
  el("tr-qty-0").focus();
}

function showResult(kind: string, text: string): void {
  const banner = el("tr-result");
  banner.className = kind ? `banner banner-${kind}` : "banner";
  banner.textContent = text;
  banner.hidden = !text;
}

async function reviewOrder(): Promise<void> {
  const u = units();
  if (!u || gate() !== null || !marketId) return renderGate();
  const steps = readSteps(u);
  if (steps === null) {
    showResult("warning", "Enter a price and a quantity first.");
    return;
  }
  // The form already says what is wrong; the core would refuse it too.
  if (typeof steps === "string" || reservation(steps).reserve > reservation(steps).available) return renderForm();
  showResult("", "");
  const id = marketId;
  try {
    const draft = await invoke<string>("trade_order_draft", {
      marketId: id,
      side,
      steps: steps.map((s) => ({ price: s.price.toString(), quantity: s.quantity.toString() })),
    });
    drafts.set(draft, "order");
    showResult("info", "Review the order in the approval window. Nothing is signed until you approve it there.");
  } catch (e) {
    showResult("danger", String(e));
  }
  renderForm();
}

async function cancelOrder(s: "bid" | "ask"): Promise<void> {
  if (gate() !== null || !marketId) return renderGate();
  try {
    const draft = await invoke<string>("trade_cancel_draft", { marketId, side: s });
    drafts.set(draft, "cancel");
    showResult("info", "Review the cancellation in the approval window.");
  } catch (e) {
    showResult("danger", String(e));
  }
  render();
}

/** Handles the approval window's decision on a draft this page opened;
 *  false when the draft is not this page's. */
export function tradeDecided(r: TradeResult): boolean {
  const what = drafts.get(r.id);
  if (!what) return false;
  drafts.delete(r.id);
  if (r.outcome === "traded" && "receipt" in r) {
    const [kind, text] = receiptText(r.receipt, what);
    showResult(kind, text);
    if (r.receipt.accepted) clearForm();
  } else if (r.outcome === "rejected") {
    showResult("warning", "Rejected. Nothing was signed.");
  } else if (r.outcome === "failed" && "error" in r) {
    showResult("danger", `Not sent: ${r.error}`);
  }
  void loadRequests();
  if (active) render();
  return true;
}

function fillText(o: Outcome): string {
  const u = units();
  if (o.fill_quantity === null || !u) return "";
  const q = BigInt(o.fill_quantity);
  if (q === 0n) return "No fill in that auction; what is left rests on the book.";
  const at = o.fill_price === null ? "" : ` at ${formatPrice(BigInt(o.fill_price), u)} B3`;
  return `Filled ${formatQuantity(q, u.decimals)} ${tokenWord()}${at}.`;
}

function receiptText(r: Receipt, what: "order" | "cancel"): [string, string] {
  if (r.state === "rejected") return ["danger", `Refused${r.reason ? `: ${r.reason}` : ""}. Nothing is pending.`];
  if (r.state === "unknown") {
    return ["warning", "Signed and kept, but the outcome is not known. Do not send a replacement: the same request may still be certified. Follow it under Requests."];
  }
  if (r.state === "certified_inclusion") {
    const o = r.outcome;
    if (o.status === "rejected") return ["danger", `Certified but not applied: ${o.text || "the market state refused it"}.`];
    if (o.status === "not_executed") return ["warning", `Certified but not executed: ${o.text || "another action used its sequence"}.`];
    if (what === "cancel") return ["success", "Cancellation certified. What the order held is back in your trading balance."];
    return ["success", `Certified. ${fillText(o)}`.trim()];
  }
  if (what === "cancel") return ["info", "Accepted: queued, not yet certified. Until it is, the order can still fill; see Requests."];
  return ["info", "Accepted: queued, not yet certified. An accepted order is not a fill; see Requests."];
}

// ---- requests ------------------------------------------------------------------

const REQUESTS_NOTE = el("tr-requests-note").textContent ?? "";

async function loadRequests(): Promise<void> {
  const id = marketId;
  if (!active || !id || requestsLoading) return;
  requestsLoading = true;
  try {
    const list = await invoke<TradeRequest[]>("trade_requests", { marketId: id });
    if (id !== marketId) return;
    requests = list;
    el("tr-requests-note").textContent = REQUESTS_NOTE;
  } catch (e) {
    if (id !== marketId) return;
    el("tr-requests-note").textContent = `Could not read your requests: ${e}`;
  } finally {
    requestsLoading = false;
    requestsRead = Date.now();
  }
  renderRequests();
  renderGate();
}

const STAGES: Record<string, string> = {
  signed: "Signed",
  sent: "Sent",
  queued: "Queued",
  admitted: "Admitted",
  certified: "Certified",
  outcome_verified: "Certified",
  not_executed: "Not executed",
  refused: "Refused",
  unknown: "Unknown",
};

function requestText(r: TradeRequest, u: Units): string {
  const verb = r.side === "bid" ? "Buy" : "Sell";
  if (r.kind === "cancel") return `Cancel ${r.side === "bid" ? "buy" : "sell"}`;
  // A request keeps the canonical curve it signed: its limit is the edge
  // that holds quantity, as for a standing order.
  const order: Order = { side: r.side ?? "bid", status: "", filled: "0", remaining: "0", reserved: "0", points: r.points };
  const prices = limits(order);
  const top = r.points.reduce((max, p) => (BigInt(p.quantity) > max ? BigInt(p.quantity) : max), 0n);
  const amount = isMasked() ? "••••" : `${formatQuantity(top, u.decimals)} ${tokenWord()}`;
  if (prices.length > 1) return `${verb} ladder, ${prices.length} prices, ${amount}`;
  return `${verb} ${amount} · limit ${prices.length ? formatPrice(prices[0], u) : "—"}`;
}

/** A verified fill in a table cell: the quantity and price only. */
function shortFill(o: Outcome): string {
  const u = units();
  if (o.fill_quantity === null || !u) return "";
  const q = BigInt(o.fill_quantity);
  if (q === 0n) return "No fill yet";
  return `Filled ${formatQuantity(q, u.decimals)}${o.fill_price === null ? "" : ` at ${formatPrice(BigInt(o.fill_price), u)}`}`;
}

function outcomeText(r: TradeRequest): string {
  const o = r.outcome;
  switch (o.status) {
    case null:
      return "—";
    case "applied":
      return r.kind === "cancel" ? "Cancelled" : shortFill(o) || "Applied";
    case "rejected":
      return `Rejected: ${o.text}`;
    case "included_not_executed":
      return "Included, not executed yet";
    case "not_executed":
      return "Not executed";
    default:
      return o.status;
  }
}

function renderRequests(): void {
  const rows = el("tr-requests-rows");
  const u = units();
  const list = requests.filter((r) => r.kind !== "other");
  if (!u) {
    rows.replaceChildren();
    return;
  }
  rows.replaceChildren(
    ...list.map((r) => {
      const tr = document.createElement("tr");
      const td = (content: string | Node, cls = "", title = "") => {
        const cell = document.createElement("td");
        if (cls) cell.className = cls;
        if (title) cell.title = title;
        cell.append(content);
        tr.append(cell);
      };
      td(r.submitted_ms ? new Date(r.submitted_ms).toLocaleTimeString() : "—", "tnum");
      td(requestText(r, u), "nowrap");
      td(r.sequence === null ? "—" : String(r.sequence), "num");
      // The node's cached stage is not renewed once the account moves on, so
      // a settled request without a verified outcome says only that.
      const takenIn = r.settled && !r.certified && r.state !== "rejected";
      const stage = r.certified ? "Certified" : takenIn ? "Taken in" : (STAGES[r.stage] ?? (r.stage || r.state || "—"));
      const badge = document.createElement("span");
      badge.className = `badge ${r.certified ? "badge-success" : r.state === "rejected" ? "badge-danger" : r.pending ? "badge-warning" : "badge-neutral"}`;
      badge.textContent = stage;
      td(badge, "", takenIn ? "The certified account has moved past its sequence. Its outcome was not verified here." : r.reason);
      td(outcomeText(r), "nowrap");
      if (r.pending && r.uncertain) {
        const ack = document.createElement("button");
        ack.type = "button";
        ack.className = "btn btn-secondary btn-sm";
        ack.textContent = "Review uncertain…";
        ack.title = "Lets new requests be sent. It does not retry, cancel or prove anything about this one.";
        let armed = false;
        ack.addEventListener("click", async () => {
          if (!armed) {
            armed = true;
            ack.textContent = "Allow new requests";
            ack.title = "This request may still be certified later. Check Your orders and balances first.";
            return;
          }
          ack.disabled = true;
          try {
            await invoke("trade_acknowledge", { marketId, actionId: r.action_id });
          } catch (e) {
            showResult("danger", String(e));
          }
          void loadRequests();
        });
        td(ack, "num");
      } else {
        td("");
      }
      return tr;
    }),
  );
}

// ---- events --------------------------------------------------------------------

el<HTMLSelectElement>("tr-market").addEventListener("change", (e) => selectMarket((e.target as HTMLSelectElement).value));

const TABS = [
  ["tr-tab-orders", "tr-orders"],
  ["tr-tab-trades", "tr-trades"],
  ["tr-tab-requests", "tr-requests"],
] as const;
for (const [tab, panel] of TABS) {
  el(tab).addEventListener("click", () => {
    for (const [t, p] of TABS) {
      el(t).setAttribute("aria-selected", String(t === tab));
      el(p).hidden = p !== panel;
    }
  });
}

for (const [id, s] of [
  ["tr-buy", "bid"],
  ["tr-sell", "ask"],
] as const) {
  el(id).addEventListener("click", () => {
    side = s;
    renderForm();
  });
}
el("tr-ladder").addEventListener("change", renderForm);
el("tr-cancel-bid").addEventListener("click", () => void cancelOrder("bid"));
el("tr-cancel-ask").addEventListener("click", () => void cancelOrder("ask"));
el("tr-form").addEventListener("input", renderForm);
el("tr-form").addEventListener("submit", (e) => {
  e.preventDefault();
  void reviewOrder();
});

// Staleness is a matter of time, not of replies: re-check it every second.
// Requests are read again while one is pending, and now and then otherwise.
setInterval(() => {
  if (active && market.status) renderHead();
  const due = requests.some((r) => r.pending) ? REQUESTS_PENDING_MS : REQUESTS_IDLE_MS;
  if (active && marketId && Date.now() - requestsRead > due) void loadRequests();
}, 1000);

/** Re-render after amounts were masked or unmasked. */
export function tradeMaskChanged(): void {
  if (active) render();
}
