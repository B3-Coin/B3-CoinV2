// The Trade page: a FlowMesh token/B3 market's verified book, clearings,
// trades and this wallet's trading account, kept live while the page is
// open. Rules from doc/flowmesh-trading-ui.md and doc/flowmesh-order-book.md:
// prices are certified clearings by microblock sequence, never invented
// times; the book shows the node's exact levels and is not recomputed here;
// unknown is never shown as zero; data older than 3 s is marked stale.
import { invoke } from "@tauri-apps/api/core";

import { amountNode, el, isMasked, shorten } from "../shared/ui";
import { B3_DECIMALS, decimal, formatB3, formatPrice, formatQuantity, quantityPlaces, spread, type Units } from "./tradefmt";

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
    // A full reply: an absent account means this wallet has none.
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
    div.title = `${level.curves} order${level.curves === 1 ? "" : "s"}${range}${own}`;
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
      td(side);
      const prices = limits(o).map((p) => formatPrice(p, u));
      td(prices.join(", ") || "—", "num");
      td(`${formatQuantity(BigInt(o.remaining), u.decimals)} ${label}`, "num");
      td(`${formatQuantity(BigInt(o.filled), u.decimals)} ${label}`, "num");
      td(o.side === "bid" ? `${formatB3(BigInt(o.reserved))} B3` : `${formatQuantity(BigInt(o.reserved), u.decimals)} ${label}`, "num");
      td(o.status === "partially-filled" ? "Partly filled" : "Open");
      if (isMasked()) for (const cell of [...tr.children].slice(2, 5)) cell.textContent = "••••";
      return tr;
    }),
  );
  note.textContent = account.orders.length
    ? "One order per side: a new Buy replaces your Buy, a new Sell your Sell. Amounts are certified."
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

// ---- events --------------------------------------------------------------------

el<HTMLSelectElement>("tr-market").addEventListener("change", (e) => selectMarket((e.target as HTMLSelectElement).value));

for (const [tab, panel] of [
  ["tr-tab-orders", "tr-orders"],
  ["tr-tab-trades", "tr-trades"],
] as const) {
  el(tab).addEventListener("click", () => {
    for (const [t, p] of [
      ["tr-tab-orders", "tr-orders"],
      ["tr-tab-trades", "tr-trades"],
    ]) {
      el(t).setAttribute("aria-selected", String(t === tab));
      el(p).hidden = p !== panel;
    }
  });
}

// Staleness is a matter of time, not of replies: re-check it every second.
setInterval(() => {
  if (active && market.status) renderHead();
}, 1000);

/** Re-render after amounts were masked or unmasked. */
export function tradeMaskChanged(): void {
  if (active) render();
}
