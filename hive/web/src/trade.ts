// The Trade page: a FlowMesh token/B3 market's verified book, clearings,
// trades and this wallet's trading account, kept live while the page is
// open. Rules from doc/flowmesh-trading-ui.md and doc/flowmesh-order-book.md:
// prices are certified clearings by microblock sequence, never invented
// times; the book shows the node's exact levels and is not recomputed here;
// unknown is never shown as zero; data older than 3 s is marked stale.
// Orders, cancellations, deposits and withdrawals are drafted by the core,
// which checks them again, and are signed only from the approval window:
// this page never signs.
import { invoke } from "@tauri-apps/api/core";

import { bannerTone, el, isMasked, shorten } from "../shared/ui";
import { pickAddress } from "./picker";
import {
  B3_DECIMALS,
  decimal,
  formatB3,
  formatPrice,
  formatQuantity,
  notional,
  parsePrice,
  parseQuantity,
  quantityPlaces,
  spread,
  type Units,
} from "./tradefmt";

// Mirrors src-tauri/src/flowmesh.rs.
type MarketSummary = { market_id: string; base_asset_id: string; vault_id: string; running: boolean; paused: boolean; halt: string; error: string };
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
type Outcome = { status: string | null; text: string; code: string; fill_quantity: string | null; fill_price: string | null };
type Receipt = { action_id: string; accepted: boolean; state: string; stage: string; reason: string; outcome: Outcome };
type TradeRequest = {
  action_id: string;
  kind: "order" | "cancel" | "withdraw" | "deposit" | "other";
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
/** Mirrors flowmesh::Deposit. asset, amount and vault_id are null when the
 *  output could not be read; such a deposit is shown, and the node checks
 *  its market when admission is asked. */
type Deposit = {
  txid: string;
  vout: number;
  confirmations: number;
  time: number;
  swept: boolean;
  asset: "b3" | "token" | null;
  amount: string | null;
  vault_id: string | null;
};
/** Mirrors flowmesh::Payout: a certified withdrawal whose payout waits to
 *  be published. destination is null when Hive cannot tell which address
 *  it pays. */
type Payout = { effect_id: string; asset: "b3" | "token"; amount: string; sequence: number; destination: string | null };
/** Mirrors flowmesh::Checkpoint: the next certified entry waiting for its
 *  checkpoint on the B3 chain, while a withdrawal of this wallet's waits. */
type Checkpoint = { checkpoint_id: string; sequence: number; waiting: number };
/** A deposit counts once it is this deep (flowmesh::DEPOSIT_CONFIRMATIONS). */
const DEPOSIT_CONFIRMATIONS = 31;
export type TradeResult =
  | { outcome: "traded"; id: string; receipt: Receipt }
  | { outcome: "deposited"; id: string; deposit: { txid: string; vout: number; fee: string } }
  | { outcome: "published"; id: string; payout: { txid: string; fee: string } }
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
/** The order form takes one price (limit) or up to four (ladder). */
let ladder = false;
/** The chart shows clearings (price) or the book's cumulative depth. */
let chartView: "price" | "depth" = "price";
/** Which sides of the book are listed. */
let bookShow: "both" | "bids" | "asks" = "both";
/** Drafts this page opened, until the approval window decides them. */
const drafts = new Map<
  string,
  { what: "order" | "cancel" | "deposit" | "withdraw" | "payout" | "checkpoint"; epoch: number; effect?: string; checkpoint?: string }
>();
/** Bumped when the wallet or market changes. A draft or read that belongs to
 *  an older epoch must not touch the view that replaced it. */
let epoch = 0;
/** A draft is being prepared: a second click or Enter must not make another. */
let preparing = false;
/** The funding form moves funds in (deposit) or out (withdraw). */
let funding: "in" | "out" = "in";
let deposits: Deposit[] = [];
let depositsRead = 0;
/** Deposits whose admission this page asked for in this session. */
const admitted = new Set<string>();
let requests: TradeRequest[] = [];
let requestsRead = 0;
/** The epoch whose request read is in flight, if any. */
let requestsLoadingEpoch: number | null = null;
/** The last deposit read failed with this message. */
let depositsError: string | null = null;
let payouts: Payout[] = [];
let payoutsError: string | null = null;
/** Payouts published from this page: effect id to transaction id. They stay
 *  listed until the transaction confirms. */
const published = new Map<string, string>();
/** The payout whose address is being asked for. */
let askingPayout: Payout | null = null;
let checkpoint: Checkpoint | null = null;
/** A checkpoint published from this page, until another is pending. */
let publishedCheckpoint: { id: string; txid: string } | null = null;

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
  epoch++;
  market = blank();
  requests = [];
  requestsRead = 0;
  deposits = [];
  depositsRead = 0;
  depositsError = null;
  payouts = [];
  payoutsError = null;
  published.clear();
  askingPayout = null;
  checkpoint = null;
  publishedCheckpoint = null;
  admitted.clear();
  clearForm();
  clearFunding();
  showResult("", "");
  showFundResult("", "");
  renderDeposits();
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
  epoch++;
  market = blank();
  requests = [];
  requestsRead = 0;
  deposits = [];
  depositsRead = 0;
  depositsError = null;
  payouts = [];
  payoutsError = null;
  published.clear();
  askingPayout = null;
  checkpoint = null;
  publishedCheckpoint = null;
  admitted.clear();
  renderDeposits();
  clearForm();
  clearFunding();
  showResult("", "");
  showFundResult("", "");
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
  renderFunding();
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

/** The newest certified clearing prices, newest first: from the ticks the
 *  page follows, or the endpoint's trade list before any arrive. */
function lastPrices(): bigint[] {
  const cleared = market.ticks.filter((t) => t.cleared).map((t) => BigInt(t.price));
  if (cleared.length) return cleared.slice(-2).reverse();
  return market.trades.slice(0, 2).map((t) => BigInt(t.price));
}

/** Up or down from the previous clearing, for colour; "" when unchanged. */
function direction(): "" | "tr-up" | "tr-down" {
  const [last, prev] = lastPrices();
  if (last === undefined || prev === undefined || last === prev) return "";
  return last > prev ? "tr-up" : "tr-down";
}

/** The change from the previous clearing in percent, to 2 decimals toward
 *  zero, computed exactly. */
function changeText(): string {
  const [last, prev] = lastPrices();
  if (last === undefined || prev === undefined || prev === 0n) return "";
  const diff = last - prev;
  const hundredths = ((diff < 0n ? -diff : diff) * 10000n) / prev;
  return `${diff > 0n ? "+" : diff < 0n ? "−" : ""}${decimal(hundredths, 2, 2)}%`;
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
  const [last] = lastPrices();
  const dir = direction();
  const lastText = last !== undefined && u ? formatPrice(last, u) : "—";
  el("tr-last").textContent = lastText;
  el("tr-last").className = `tr-last tnum ${dir}`;
  el("tr-change").textContent = changeText();
  el("tr-change").className = `tr-change tnum ${dir}`;
  el("tr-mid-last").textContent = last !== undefined && u ? `${lastText} ${dir === "tr-up" ? "↑" : dir === "tr-down" ? "↓" : ""}`.trim() : "—";
  el("tr-mid-last").className = `tr-mid-last tnum ${dir}`;
  const cleared = market.ticks.filter((t) => t.cleared).map((t) => BigInt(t.price));
  if (cleared.length && u) {
    const hi = cleared.reduce((a, b) => (b > a ? b : a));
    const lo = cleared.reduce((a, b) => (b < a ? b : a));
    el("tr-range").textContent = hi === lo ? formatPrice(hi, u) : `${formatPrice(lo, u)} – ${formatPrice(hi, u)}`;
  } else {
    el("tr-range").textContent = "—";
  }
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
  // One side alone gets the room of both.
  const rows = bookShow === "both" ? BOOK_ROWS : 2 * BOOK_ROWS;
  const askLevels = (book.asks?.levels ?? []).slice(0, rows);
  const bidLevels = (book.bids?.levels ?? []).slice(0, rows);
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
    // A button, so the keyboard can take a level's price too.
    const div = document.createElement("button");
    div.type = "button";
    div.dataset.level = `${side}:${price}`;
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
    div.setAttribute(
      "aria-label",
      `${side === "asks" ? "Ask" : "Bid"} ${formatPrice(BigInt(price), u)} B3, size ${formatQuantity(BigInt(level.size), u.decimals, places)}${own ? ", includes yours" : ""}. Use this price.`,
    );
    div.classList.add("tr-pick");
    div.addEventListener("click", () => pickPrice(BigInt(price)));
    return div;
  };
  // Redrawing must not take the keyboard focus off the level it was on.
  const focused = (document.activeElement as HTMLElement | null)?.dataset.level;
  // Asks above the spread, best (lowest) nearest it.
  asks.replaceChildren(...[...askLevels].reverse().map((l) => row(l, "asks", shown.asks)));
  bids.replaceChildren(...bidLevels.map((l) => row(l, "bids", shown.bids)));
  if (focused) el("tr-book").querySelector<HTMLElement>(`[data-level="${focused}"]`)?.focus();
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
const [CHART_W, CHART_H, CHART_PAD] = [600, 260, 14];
/** Clearings take the top of the chart; their volumes the bottom. */
const PRICE_BOTTOM = CHART_H * 0.76;
/** Room on the right for the last-price tag. */
const TAG_ROOM = 64;

function svg(tag: string, attrs: Record<string, string>): SVGElement {
  const node = document.createElementNS(SVG, tag);
  for (const [k, v] of Object.entries(attrs)) node.setAttribute(k, v);
  return node;
}

/** Where each clearing of the price chart sits, for the hover readout. */
let plotted: { x: number; t: Tick }[] = [];

function renderChart(): void {
  const price = chartView === "price";
  // SVG elements have no hidden property: set the attribute.
  el("tr-chart").toggleAttribute("hidden", !price);
  el("tr-depth").toggleAttribute("hidden", price);
  el("tr-view-price").setAttribute("aria-selected", String(price));
  el("tr-view-depth").setAttribute("aria-selected", String(!price));
  const caption = el("tr-chart-caption");
  caption.textContent = price ? "Certified clearings by microblock" : "Standing orders by price";
  caption.title = price
    ? "Each point is a certified auction that matched. Microblocks carry no execution time, so the axis is the microblock sequence."
    : "Cumulative size of the verified standing orders at each price, computed by your node. Not certified; queued orders are not included.";
  hideCross();
  if (price) renderPriceChart();
  else renderDepthChart();
}

function clearChartLabels(): void {
  for (const id of ["tr-chart-high", "tr-chart-low", "tr-chart-first", "tr-chart-mid", "tr-chart-last"]) el(id).textContent = "";
  el("tr-lasttag").hidden = true;
}

function renderPriceChart(): void {
  const chart = el("tr-chart");
  const u = units();
  const ticks = market.ticks;
  const cleared = ticks.filter((t) => t.cleared);
  const empty = el("tr-chart-empty");
  empty.textContent = "No auction has matched yet in the retained history.";
  empty.hidden = cleared.length > 0;
  plotted = [];
  if (!u || !cleared.length) {
    chart.replaceChildren();
    chart.setAttribute("aria-label", "Clearing prices: no auction has matched yet");
    clearChartLabels();
    return;
  }
  const [W, H, pad] = [CHART_W, CHART_H, CHART_PAD];
  const first = ticks[0].sequence;
  const lastSeq = ticks[ticks.length - 1].sequence;
  const prices = cleared.map((t) => BigInt(t.price));
  const hi = prices.reduce((a, b) => (b > a ? b : a));
  const lo = prices.reduce((a, b) => (b < a ? b : a));
  // Plot positions only; every label is formatted exactly from the BigInt.
  const span = Number(hi - lo) || 1;
  const right = W - TAG_ROOM;
  const x = (seq: number) => (lastSeq === first ? right / 2 : ((seq - first) / (lastSeq - first)) * (right - 2 * pad) + pad);
  const y = (p: bigint) => (hi === lo ? PRICE_BOTTOM / 2 : PRICE_BOTTOM - pad - (Number(p - lo) / span) * (PRICE_BOTTOM - 2 * pad));

  const defs = svg("defs", {});
  const grad = svg("linearGradient", { id: "tr-fill", x1: "0", y1: "0", x2: "0", y2: "1" });
  grad.append(
    // currentColor is the chart's colour, the accent (see .tr-chart svg).
    svg("stop", { offset: "0%", "stop-color": "currentColor", "stop-opacity": "0.3" }),
    svg("stop", { offset: "100%", "stop-color": "currentColor", "stop-opacity": "0" }),
  );
  defs.append(grad);
  const grid = [0.2, 0.4, 0.6, 0.8].map((f) =>
    svg("line", { class: "grid", x1: "0", x2: String(W), y1: (PRICE_BOTTOM * f).toFixed(1), y2: (PRICE_BOTTOM * f).toFixed(1) }),
  );
  // A step line: the price holds until the next clearing, and to now.
  const points: string[] = [];
  cleared.forEach((t, i) => {
    if (i > 0) points.push(`${x(t.sequence).toFixed(1)},${y(prices[i - 1]).toFixed(1)}`);
    points.push(`${x(t.sequence).toFixed(1)},${y(prices[i]).toFixed(1)}`);
  });
  const lastY = y(prices[prices.length - 1]);
  points.push(`${right.toFixed(1)},${lastY.toFixed(1)}`);
  const area = svg("polygon", {
    class: "area",
    fill: "url(#tr-fill)",
    points: `${x(cleared[0].sequence).toFixed(1)},${PRICE_BOTTOM} ${points.join(" ")} ${right.toFixed(1)},${PRICE_BOTTOM}`,
  });
  const line = svg("polyline", { class: "line", points: points.join(" ") });
  const lastLine = svg("line", { class: "lastline", x1: "0", x2: String(W), y1: lastY.toFixed(1), y2: lastY.toFixed(1) });
  // Volumes, on their own scale below the prices.
  const volumes = cleared.map((t) => BigInt(t.volume));
  const maxVol = volumes.reduce((a, b) => (b > a ? b : a), 0n);
  const slot = lastSeq === first ? 12 : ((right - 2 * pad) / (lastSeq - first)) * 0.6;
  const barW = Math.max(2, Math.min(12, slot));
  const volRoom = H - PRICE_BOTTOM - 4;
  const bars = cleared.map((t, i) => {
    const h = maxVol > 0n ? (Number((volumes[i] * 1000n) / maxVol) / 1000) * volRoom : 0;
    return svg("rect", { class: "vol", x: (x(t.sequence) - barW / 2).toFixed(1), y: (H - h).toFixed(1), width: barW.toFixed(1), height: Math.max(1, h).toFixed(1) });
  });
  const dots =
    cleared.length <= 60 ? cleared.map((t, i) => svg("circle", { class: "dot", cx: x(t.sequence).toFixed(1), cy: y(prices[i]).toFixed(1), r: "2.5", fill: "var(--accent)" })) : [];
  chart.replaceChildren(defs, ...grid, ...bars, area, line, lastLine, ...dots);
  plotted = cleared.map((t) => ({ x: x(t.sequence), t }));

  el("tr-chart-high").textContent = formatPrice(hi, u);
  el("tr-chart-low").textContent = hi === lo ? "" : formatPrice(lo, u);
  const tag = el("tr-lasttag");
  tag.textContent = formatPrice(prices[prices.length - 1], u);
  tag.style.top = `${((lastY / H) * 100).toFixed(2)}%`;
  tag.hidden = false;
  el("tr-chart-first").textContent = `#${first}`;
  el("tr-chart-mid").textContent = `${cleared.length} clearing${cleared.length === 1 ? "" : "s"}`;
  el("tr-chart-last").textContent = `#${lastSeq}`;
  // The pointer reads single points; this says the whole chart at once.
  chart.setAttribute(
    "aria-label",
    `Clearing prices in B3: ${cleared.length} clearing${cleared.length === 1 ? "" : "s"} from microblock ${first} to ${lastSeq}, ` +
      `last ${formatPrice(prices[prices.length - 1], u)}, high ${formatPrice(hi, u)}, low ${formatPrice(lo, u)}`,
  );
}

function renderDepthChart(): void {
  const chart = el("tr-depth");
  const u = units();
  const book = market.book;
  plotted = [];
  clearChartLabels();
  const bids = book?.available ? (book.bids?.levels ?? []) : [];
  const asks = book?.available ? (book.asks?.levels ?? []) : [];
  const empty = el("tr-chart-empty");
  empty.textContent = book && !book.available ? `Levels unavailable (${book.reason || "unknown"}); read again shortly.` : "No standing orders.";
  empty.hidden = !!u && (bids.length > 0 || asks.length > 0);
  if (!u || (!bids.length && !asks.length)) {
    chart.replaceChildren();
    chart.setAttribute("aria-label", "Book depth: no standing orders");
    return;
  }
  const [W, H, pad] = [CHART_W, CHART_H, CHART_PAD];
  // Every order in a level accepts its label price: a bid's lower edge, an
  // ask's upper. Levels come best first.
  const b = bids.map((l) => ({ p: BigInt(l.low), c: BigInt(l.cumulative) }));
  const a = asks.map((l) => ({ p: BigInt(l.high), c: BigInt(l.cumulative) }));
  chart.setAttribute(
    "aria-label",
    `Book depth: ${b.length} bid level${b.length === 1 ? "" : "s"}${b.length ? `, best ${formatPrice(b[0].p, u)} B3` : ""}; ` +
      `${a.length} ask level${a.length === 1 ? "" : "s"}${a.length ? `, best ${formatPrice(a[0].p, u)} B3` : ""}`,
  );
  const all = [...b, ...a];
  let pLo = all.reduce((m, v) => (v.p < m ? v.p : m), all[0].p);
  let pHi = all.reduce((m, v) => (v.p > m ? v.p : m), all[0].p);
  if (pLo === pHi) {
    pLo = pLo > 10n * u.tick ? pLo - 10n * u.tick : 0n;
    pHi = pHi + 10n * u.tick;
  }
  const cMax = all.reduce((m, v) => (v.c > m ? v.c : m), 1n);
  const x = (p: bigint) => pad + (Number(p - pLo) / Number(pHi - pLo)) * (W - 2 * pad);
  const y = (c: bigint) => H - pad - (Number((c * 1000n) / cMax) / 1000) * (H - 2 * pad);
  const bottom = (H - pad).toFixed(1);
  const path = (levels: { p: bigint; c: bigint }[], end: bigint): string => {
    const pts: string[] = [`${x(levels[0].p).toFixed(1)},${bottom}`];
    levels.forEach((l, i) => {
      if (i > 0) pts.push(`${x(l.p).toFixed(1)},${y(levels[i - 1].c).toFixed(1)}`);
      pts.push(`${x(l.p).toFixed(1)},${y(l.c).toFixed(1)}`);
    });
    pts.push(`${x(end).toFixed(1)},${y(levels[levels.length - 1].c).toFixed(1)}`);
    return pts.join(" ");
  };
  const grid = [0.25, 0.5, 0.75].map((f) => svg("line", { class: "grid", x1: "0", x2: String(W), y1: String(H * f), y2: String(H * f) }));
  const shapes: SVGElement[] = [];
  if (b.length) {
    const line = path(b, pLo);
    shapes.push(svg("polygon", { class: "bidarea", points: `${line} ${x(pLo).toFixed(1)},${bottom}` }), svg("polyline", { class: "bidline", points: line }));
  }
  if (a.length) {
    const line = path(a, pHi);
    shapes.push(svg("polygon", { class: "askarea", points: `${line} ${x(pHi).toFixed(1)},${bottom}` }), svg("polyline", { class: "askline", points: line }));
  }
  chart.replaceChildren(...grid, ...shapes);
  el("tr-chart-high").textContent = `${formatQuantity(cMax, u.decimals)} ${tokenWord()}`;
  el("tr-chart-first").textContent = formatPrice(pLo, u);
  el("tr-chart-mid").textContent = `${b.length} bid and ${a.length} ask level${a.length === 1 ? "" : "s"}`;
  el("tr-chart-last").textContent = formatPrice(pHi, u);
}

function hideCross(): void {
  el("tr-cross").hidden = true;
  el("tr-tip").hidden = true;
}

/** The clearing nearest the pointer: a line through it and its numbers. */
function hoverChart(e: MouseEvent): void {
  const u = units();
  if (chartView !== "price" || !plotted.length || !u) return hideCross();
  const box = el("tr-chart-box").getBoundingClientRect();
  const vx = ((e.clientX - box.left) / box.width) * CHART_W;
  const near = plotted.reduce((best, p) => (Math.abs(p.x - vx) < Math.abs(best.x - vx) ? p : best));
  const left = (near.x / CHART_W) * box.width;
  const cross = el("tr-cross");
  cross.style.left = `${left}px`;
  cross.hidden = false;
  const tip = el("tr-tip");
  const row = (k: string, v: string) => {
    const div = document.createElement("div");
    const key = document.createElement("span");
    key.className = "muted";
    key.textContent = `${k} `;
    div.append(key, v);
    return div;
  };
  tip.replaceChildren(
    row("Microblock", `#${near.t.sequence}`),
    row("Price", `${formatPrice(BigInt(near.t.price), u)} B3`),
    row("Volume", `${formatQuantity(BigInt(near.t.volume), u.decimals)} ${tokenWord()}`),
  );
  tip.hidden = false;
  const flip = left > box.width / 2;
  tip.style.left = flip ? "" : `${left + 10}px`;
  tip.style.right = flip ? `${box.width - left + 10}px` : "";
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
  const open = market.account?.orders.filter((o) => o.remaining !== "0").length ?? 0;
  el("tr-orders-count").textContent = String(open);
  el("tr-orders-count").hidden = open === 0;
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
  el("tr-base-free-label").textContent = label;
  el("tr-no-account").hidden = !market.accountKnown || account !== null;
  el("tr-balance").hidden = market.accountKnown && account === null;
  const b3 = (id: string, atoms: string | undefined) => {
    el(id).textContent = atoms === undefined ? "—" : isMasked() ? "••••" : `${formatB3(BigInt(atoms))} B3`;
  };
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
  return ladder ? LADDER_STEPS : 1;
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
    if (!priceText || !quantityText) return `${where}${where ? "enter" : "Enter"} both a price and an amount.`;
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

/** A token quantity as typed into the form: exact, without grouping. */
function plainQuantity(q: bigint, u: Units): string {
  return formatQuantity(q, u.decimals).replace(/\u202f/g, "");
}

/** The most the first row can hold: what this order may reserve (the free
 *  balance plus what the order it replaces holds), at the entered price for
 *  a buy, on the lot grid. Null while it cannot be known. */
function maxQuantity(u: Units): bigint | null {
  if (!market.account) return null;
  const { available } = reservation([]);
  if (side === "ask") return (available / u.lot) * u.lot;
  const price = parsePrice(el<HTMLInputElement>("tr-price-0").value.trim(), u);
  if ("error" in price) return null;
  return (available / price.value / u.lot) * u.lot;
}

/** Sets the first row's amount to a share of the most it can hold. */
function setShare(pct: number): void {
  const u = units();
  if (!u || ladder) return;
  const max = maxQuantity(u);
  if (max === null) {
    showResult("warning", side === "bid" ? "Enter a price first: the share is of what your balance buys at it." : "There is no trading balance to sell from.");
    return renderForm();
  }
  const q = ((max * BigInt(pct)) / 100n / u.lot) * u.lot;
  el<HTMLInputElement>("tr-qty-0").value = q > 0n ? plainQuantity(q, u) : "";
  renderForm();
}

/** Moves a price or amount one step on its grid. An empty price starts
 *  from the best price on the other side, if the book is current. */
function nudge(id: string, dir: number): void {
  const u = units();
  if (!u) return;
  const input = el<HTMLInputElement>(id);
  const isPrice = id.startsWith("tr-price");
  const unit = isPrice ? u.tick : u.lot;
  const text = input.value.trim();
  let v = 0n;
  if (text) {
    const parsed = isPrice ? parsePrice(text, u) : parseQuantity(text, u);
    if ("error" in parsed) return renderForm();
    v = parsed.value;
  } else if (isPrice) {
    const best = side === "bid" ? market.book?.best_ask : market.book?.best_bid;
    const start = best ? BigInt(best) : lastPrices()[0];
    if (start !== undefined && !stale()) {
      input.value = plainPrice(start, u);
      return renderForm();
    }
  }
  v += BigInt(dir) * unit;
  if (v < unit) v = unit;
  input.value = isPrice ? plainPrice(v, u) : plainQuantity(v, u);
  renderForm();
}

function renderForm(): void {
  const u = units();
  const token = tokenWord();
  const buy = side === "bid";
  const ticket = el("tr-ticket");
  ticket.dataset.side = side;
  ticket.dataset.type = ladder ? "ladder" : "limit";
  el("tr-buy").setAttribute("aria-selected", String(buy));
  el("tr-sell").setAttribute("aria-selected", String(!buy));
  el("tr-type-limit").setAttribute("aria-selected", String(!ladder));
  el("tr-type-ladder").setAttribute("aria-selected", String(ladder));
  el("tr-review").textContent = `${buy ? "Buy" : "Sell"} ${token}`;
  for (const unit of document.querySelectorAll("#tr-ticket [data-tr-unit]")) unit.textContent = token;
  for (let i = 1; i < LADDER_STEPS; i++) el(`tr-step-${i}`).hidden = !ladder;
  el("tr-steps-help").textContent = u
    ? `Steps: ${formatPrice(u.tick, u)} B3 per ${token} · ${formatQuantity(u.lot, u.decimals)} ${token}. ${buy ? "Buys at your price or below" : "Sells at your price or above"}, at one uniform auction price. Sellers pay 0.01% of the matched B3.`
    : "";
  const replaced = openOrder(side);
  const money = (v: bigint) => (u ? (buy ? `${formatB3(v)} B3` : `${formatQuantity(v, u.decimals)} ${token}`) : "—");
  const avbl = el("tr-avbl");
  avbl.textContent = !market.account || !u ? "—" : isMasked() ? "••••" : money(reservation([]).available);
  avbl.title = replaced ? `Includes what your open ${buy ? "buy" : "sell"} order holds: a new order replaces it.` : "";

  const error = el("tr-form-error");
  const steps = u ? readSteps(u) : null;
  error.textContent = typeof steps === "string" ? steps : "";
  error.hidden = typeof steps !== "string";
  const total = el("tr-total");
  el("tr-total-label").textContent = buy ? "Total, at most" : "Total, at least";
  el("tr-total-label").title = buy ? "Your limit prices times the amounts. The auction price can be lower." : "Your limit prices times the amounts, before the 0.01% seller fee. The auction price can be higher.";
  if (!u || steps === null || typeof steps === "string") {
    total.textContent = "—";
    el("tr-replaces-row").hidden = true;
  } else {
    total.textContent = isMasked() ? "•••• B3" : `${formatB3(steps.reduce((sum, s) => sum + notional(s.quantity, s.price), 0n))} B3`;
    el("tr-replaces-row").hidden = !replaced;
    if (replaced) {
      el("tr-replaces").textContent = isMasked() ? "••••" : `open ${buy ? "buy" : "sell"}, ${formatQuantity(BigInt(replaced.remaining), u.decimals)} ${token} left`;
    }
    if (market.account) {
      const { reserve, available } = reservation(steps);
      if (reserve > available) {
        error.textContent = buy
          ? "This order costs more B3 than your trading balance has available."
          : `This order sells more ${token} than your trading balance has available.`;
        error.hidden = false;
      }
    }
  }

  // The share slider follows the first row's amount.
  const slider = el<HTMLInputElement>("tr-pct");
  const max = u && !ladder ? maxQuantity(u) : null;
  let pct = 0;
  if (u && max !== null && max > 0n) {
    const q = parseQuantity(el<HTMLInputElement>("tr-qty-0").value.trim(), u);
    if (!("error" in q)) pct = Math.min(100, Number((q.value * 100n) / max));
  }
  slider.disabled = !u || max === null || max === 0n || gate() !== null;
  slider.value = String(pct);
  el("tr-slider").style.setProperty("--pct", `${pct}%`);
  el("tr-pct-out").textContent = `${pct}%`;
  for (const mark of document.querySelectorAll<HTMLElement>("#tr-slider [data-pct]")) mark.classList.toggle("on", pct >= Number(mark.dataset.pct) && pct > 0);
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
  bannerTone(banner, kind);
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
  if (preparing) return;
  preparing = true;
  showResult("", "");
  const id = marketId;
  const at = epoch;
  try {
    const draft = await invoke<string>("trade_order_draft", {
      marketId: id,
      side,
      steps: steps.map((s) => ({ price: s.price.toString(), quantity: s.quantity.toString() })),
    });
    drafts.set(draft, { what: "order", epoch: at });
    showResult("info", "Review the order in the approval window. Nothing is signed until you approve it there.");
  } catch (e) {
    showResult("danger", String(e));
  } finally {
    preparing = false;
  }
  renderForm();
}

async function cancelOrder(s: "bid" | "ask"): Promise<void> {
  if (gate() !== null || !marketId) return renderGate();
  if (preparing) return;
  preparing = true;
  const at = epoch;
  try {
    const draft = await invoke<string>("trade_cancel_draft", { marketId, side: s });
    drafts.set(draft, { what: "cancel", epoch: at });
    showResult("info", "Review the cancellation in the approval window.");
  } catch (e) {
    showResult("danger", String(e));
  } finally {
    preparing = false;
  }
  render();
}

/** Handles the approval window's decision on a draft this page opened;
 *  false when the draft is not this page's. */
export function tradeDecided(r: TradeResult): boolean {
  const draft = drafts.get(r.id);
  if (!draft) return false;
  const { what } = draft;
  drafts.delete(r.id);
  if (draft.epoch !== epoch) {
    // Decided after the wallet or market changed: say so, but leave the form
    // and results of the current view alone.
    if (active) {
      const done = r.outcome === "rejected" ? "was rejected" : r.outcome === "failed" ? "failed" : "was submitted";
      showResult("info", `A request made for another wallet or market ${done}. Check Requests there.`);
      render();
    }
    return true;
  }
  const result = what === "deposit" || what === "withdraw" || what === "payout" || what === "checkpoint" ? showFundResult : showResult;
  if (r.outcome === "traded" && "receipt" in r) {
    const [kind, text] = what === "withdraw" ? withdrawText(r.receipt) : receiptText(r.receipt, what === "cancel" ? "cancel" : "order");
    result(kind, text);
    if (r.receipt.accepted) (what === "withdraw" ? clearFunding : clearForm)();
  } else if (r.outcome === "deposited" && "deposit" in r) {
    result(
      "success",
      `Deposit broadcast (network fee ${r.deposit.fee} B3). It counts after ${DEPOSIT_CONFIRMATIONS} confirmations and admission; it is listed under Your deposits.`,
    );
    clearFunding();
    void loadDeposits();
  } else if (r.outcome === "published" && "payout" in r && what === "checkpoint") {
    if (draft.checkpoint) publishedCheckpoint = { id: draft.checkpoint, txid: r.payout.txid };
    result(
      "success",
      `Checkpoint broadcast (network fee ${r.payout.fee} B3). Once it confirms, the next checkpoint or your payout can be published here.`,
    );
    void loadDeposits();
  } else if (r.outcome === "published" && "payout" in r) {
    if (draft.effect) published.set(draft.effect, r.payout.txid);
    askingPayout = null;
    result(
      "success",
      `Payout broadcast (network fee ${r.payout.fee} B3). It is paid once the transaction confirms on the B3 chain; it leaves the list then.`,
    );
    void loadDeposits();
  } else if (r.outcome === "rejected") {
    result("warning", "Rejected. Nothing was signed.");
  } else if (r.outcome === "failed" && "error" in r) {
    result("danger", `Not sent: ${r.error}`);
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

/** Why the market refused a certified request. The node's text for a state
 *  refusal is the same for every kind; its cause depends on the kind, as in
 *  the node's RejectedByStateText. No trailing full stop. */
function refusal(o: Outcome, kind: string): string {
  if (o.code === "REJECTED_BY_STATE") {
    if (kind === "withdraw") return "not enough free balance, or the market's vault cannot pay that much yet. Nothing left your balance";
    if (kind === "cancel") return "there was nothing to cancel on that side";
    if (kind === "order") return "the funds could not be reserved or the prices were not valid. Nothing is held for it";
  }
  return (o.text || "the market state refused it").replace(/\.+$/, "");
}

function receiptText(r: Receipt, what: "order" | "cancel"): [string, string] {
  if (r.state === "rejected") return ["danger", `Refused${r.reason ? `: ${r.reason}` : ""}. Nothing is pending.`];
  if (r.state === "unknown") {
    return ["warning", "Signed and kept, but the outcome is not known. Do not send a replacement: the same request may still be certified. Follow it under Requests."];
  }
  if (r.state === "certified_inclusion") {
    const o = r.outcome;
    if (o.status === "rejected") return ["danger", `Refused: ${refusal(o, what)}.`];
    if (o.status === "not_executed") return ["warning", `Certified but not executed: ${(o.text || "another action used its sequence").replace(/\.+$/, "")}.`];
    if (what === "cancel") return ["success", "Cancellation certified. What the order held is back in your trading balance."];
    return ["success", `Certified. ${fillText(o)}`.trim()];
  }
  if (what === "cancel") return ["info", "Accepted: queued, not yet certified. Until it is, the order can still fill; see Requests."];
  return ["info", "Accepted: queued, not yet certified. An accepted order is not a fill; see Requests."];
}

// ---- deposits and withdrawals ----------------------------------------------------

/** Why a deposit or withdrawal cannot be drafted now, or null. */
function fundGate(): string | null {
  const s = market.status;
  if (!s || !market.accountKnown) return "Loading the market…";
  if (s.halt && s.halt !== "none") return `This market is halted (${s.halt}).`;
  if (s.paused) return "This market is paused; deposits and withdrawals wait.";
  if (drafts.size) return "Waiting for your decision in the approval window.";
  if (funding === "out") {
    if (!market.account) return "This wallet has no trading account in this market, so there is nothing to withdraw.";
    if (stale()) return "Withdrawals wait for a verified update: there has been none for over 3 seconds.";
    if (requests.some((r) => r.pending)) return "Your previous request is not certified yet. New requests wait for it; see Requests.";
  }
  return null;
}

function fundAsset(): "b3" | "token" {
  return el<HTMLSelectElement>("tr-fund-asset").value === "token" ? "token" : "b3";
}

/** The amount typed, exactly, in B3 atoms or token base units. */
function fundAmount(): bigint | string | null {
  const text = el<HTMLInputElement>("tr-fund-amount").value.trim();
  if (!text) return null;
  const token = fundAsset() === "token";
  const decimals = token ? market.metadata?.decimals : B3_DECIMALS;
  if (decimals === null || decimals === undefined) return "The token's decimals are not verified, so the amount cannot be checked.";
  const p = parseQuantity(text, { decimals, tick: 1n, lot: 1n });
  if ("error" in p) {
    return p.error.startsWith("The quantity must be a multiple")
      ? `${token ? tokenWord() : "B3"} amounts have at most ${decimals} decimal places.`
      : "Enter the amount as a number, like 12.5.";
  }
  return p.value;
}

function fundAvailable(): bigint | null {
  const a = market.account;
  if (!a) return null;
  return BigInt(fundAsset() === "token" ? a.base_available : a.b3_available);
}

function renderFunding(): void {
  const out = funding === "out";
  el("tr-fund-in").setAttribute("aria-selected", String(!out));
  el("tr-fund-out").setAttribute("aria-selected", String(out));
  el("tr-fund-token").textContent = tokenWord();
  el("tr-fund-to-field").hidden = !out;
  el("tr-fund-review").textContent = out ? "Review withdrawal" : "Review deposit";
  el("tr-fund-help").textContent = out
    ? "A withdrawal is a request: once certified it leaves your trading balance. After a checkpoint you publish its payout here, for a small network fee. What open orders hold cannot be withdrawn."
    : `A deposit is a B3 transaction into the market's vault. It counts after ${DEPOSIT_CONFIRMATIONS} confirmations, once the market admits it.` +
      (market.accountKnown && !market.account ? " The first one creates this wallet's trading account key: back up the wallet afterwards." : "");
  const amount = fundAmount();
  let error = typeof amount === "string" ? amount : "";
  const available = fundAvailable();
  if (!error && out && typeof amount === "bigint" && available !== null && amount > available) {
    error = "That is more than the trading balance has available.";
  }
  el("tr-fund-error").textContent = error;
  el("tr-fund-error").hidden = !error;
  const reason = fundGate();
  el("tr-fund-gate").textContent = reason ?? "";
  el("tr-fund-gate").hidden = reason === null;
  el<HTMLButtonElement>("tr-fund-review").disabled = reason !== null || amount === null || !!error;
}

function showFundResult(kind: string, text: string): void {
  const banner = el("tr-fund-result");
  bannerTone(banner, kind);
  banner.textContent = text;
  banner.hidden = !text;
}

function clearFunding(): void {
  el<HTMLInputElement>("tr-fund-amount").value = "";
  el<HTMLInputElement>("tr-fund-to").value = "";
  renderFunding();
}

async function reviewFunding(): Promise<void> {
  const amount = fundAmount();
  if (fundGate() !== null || !marketId || typeof amount !== "bigint") return renderFunding();
  if (preparing) return;
  preparing = true;
  showFundResult("", "");
  const out = funding === "out";
  const at = epoch;
  try {
    const draft = out
      ? await invoke<string>("trade_withdraw_draft", {
          marketId,
          asset: fundAsset(),
          amount: amount.toString(),
          destination: el<HTMLInputElement>("tr-fund-to").value.trim(),
        })
      : await invoke<string>("trade_deposit_draft", { marketId, asset: fundAsset(), amount: amount.toString() });
    drafts.set(draft, { what: out ? "withdraw" : "deposit", epoch: at });
    showFundResult("info", `Review the ${out ? "withdrawal" : "deposit"} in the approval window. Nothing is signed until you approve it there.`);
  } catch (e) {
    showFundResult("danger", String(e));
  } finally {
    preparing = false;
  }
  render();
}

function withdrawText(r: Receipt): [string, string] {
  if (r.state === "rejected") return ["danger", `Refused${r.reason ? `: ${r.reason}` : ""}. Nothing is pending.`];
  if (r.state === "unknown") {
    return ["warning", "Signed and kept, but the outcome is not known. Do not request it again: the same request may still be certified. Follow it under Requests."];
  }
  if (r.state === "certified_inclusion") {
    const o = r.outcome;
    if (o.status === "rejected") return ["danger", `Withdrawal refused: ${refusal(o, "withdraw")}.`];
    if (o.status === "not_executed") return ["warning", `Certified but not executed: ${(o.text || "another action used its sequence").replace(/\.+$/, "")}.`];
    return ["success", "Withdrawal certified: it has left your trading balance. It is not paid yet: once a checkpoint confirms it on the B3 chain, publish its payout under Withdrawal payouts below."];
  }
  return ["info", "Accepted: queued, not yet certified. Nothing has left your trading balance yet; see Requests."];
}

async function loadDeposits(): Promise<void> {
  const mine = generation;
  const id = marketId;
  if (!id) return;
  depositsRead = Date.now();
  const [dep, pay] = await Promise.allSettled([
    invoke<Deposit[]>("trade_deposits", { marketId: id }),
    invoke<{ payouts: Payout[]; checkpoint: Checkpoint | null }>("trade_payouts", { marketId: id }),
  ]);
  if (mine !== generation) return;
  // An empty list must not be mistaken for "none".
  if (dep.status === "fulfilled") {
    deposits = dep.value;
    depositsError = null;
  } else {
    depositsError = String(dep.reason);
  }
  if (pay.status === "fulfilled") {
    payouts = pay.value.payouts;
    checkpoint = pay.value.checkpoint;
    payoutsError = null;
    if (publishedCheckpoint && checkpoint?.checkpoint_id !== publishedCheckpoint.id) publishedCheckpoint = null;
    for (const effect of [...published.keys()]) if (!payouts.some((p) => p.effect_id === effect)) published.delete(effect);
    if (askingPayout && !payouts.some((p) => p.effect_id === askingPayout?.effect_id)) askingPayout = null;
  } else {
    payoutsError = String(pay.reason);
  }
  renderDeposits();
}

async function admit(d: Deposit, button: HTMLButtonElement): Promise<void> {
  if (!marketId) return;
  button.disabled = true;
  const key = `${d.txid}:${d.vout}`;
  try {
    const r = await invoke<Receipt>("trade_admit", { marketId, txid: d.txid, vout: d.vout });
    if (r.state === "rejected") {
      showFundResult("danger", `Admission refused${r.reason ? `: ${r.reason}` : ""}.`);
    } else {
      admitted.add(key);
      showFundResult(
        r.state === "certified_inclusion" ? "success" : "info",
        r.state === "certified_inclusion"
          ? "Deposit admitted and certified: it is in your trading balance."
          : "Admission requested. It is in your trading balance once certified; that is not yet settled on the B3 chain.",
      );
    }
  } catch (e) {
    showFundResult("danger", String(e));
  }
  void loadRequests();
  void loadDeposits();
}

/** A deposit's or payout's amount, from B3 atoms or token base units: the
 *  token in its verified decimals, or base units when they are not. */
function depositAmount(d: { asset: "b3" | "token" | null; amount: string | null }): Node {
  if (d.amount === null) {
    const unknown = document.createElement("span");
    unknown.className = "muted";
    unknown.textContent = "Amount unknown";
    return unknown;
  }
  const span = document.createElement("span");
  const label = d.asset === "b3" ? "B3" : tokenLabel(market.metadata, markets.find((m) => m.market_id === marketId)?.base_asset_id);
  const decimals = d.asset === "b3" ? B3_DECIMALS : market.metadata?.decimals;
  if (isMasked()) {
    span.textContent = `•••• ${label}`;
    span.setAttribute("aria-label", "Amount hidden");
  } else if (decimals === null || decimals === undefined) {
    span.textContent = `${d.amount} base units of ${label}`;
  } else {
    const q = BigInt(d.amount);
    span.textContent = `${d.asset === "b3" ? formatB3(q) : formatQuantity(q, decimals, quantityPlaces([q], decimals))} ${label}`;
  }
  return span;
}

async function reviewPayout(p: Payout, destination: string): Promise<void> {
  if (!marketId || preparing) return;
  preparing = true;
  showFundResult("", "");
  const at = epoch;
  try {
    const draft = await invoke<string>("trade_payout_draft", { marketId, effectId: p.effect_id, destination });
    drafts.set(draft, { what: "payout", epoch: at, effect: p.effect_id });
    showFundResult("info", "Review the payout in the approval window. Nothing is signed until you approve it there.");
  } catch (e) {
    showFundResult("danger", String(e));
  } finally {
    preparing = false;
  }
  render();
}

async function reviewCheckpoint(c: Checkpoint): Promise<void> {
  if (!marketId || preparing) return;
  preparing = true;
  showFundResult("", "");
  const at = epoch;
  try {
    const draft = await invoke<string>("trade_checkpoint_draft", { marketId });
    drafts.set(draft, { what: "checkpoint", epoch: at, checkpoint: c.checkpoint_id });
    showFundResult("info", "Review the checkpoint in the approval window. Nothing is signed until you approve it there.");
  } catch (e) {
    showFundResult("danger", String(e));
  } finally {
    preparing = false;
  }
  render();
}

function checkpointRow(c: Checkpoint): HTMLElement {
  const drow = document.createElement("div");
  drow.className = "drow";
  const k = document.createElement("span");
  k.className = "dkey";
  k.textContent = c.waiting === 1 ? "1 withdrawal waits for a checkpoint" : `${c.waiting} withdrawals wait for a checkpoint`;
  const sub = document.createElement("span");
  sub.className = "muted";
  sub.textContent = ` · entry ${c.sequence}`;
  sub.title = `Checkpoint ${c.checkpoint_id}`;
  k.append(sub);
  const v = document.createElement("span");
  v.className = "dval";
  if (publishedCheckpoint?.id === c.checkpoint_id) {
    v.textContent = "Published, confirming";
    v.title = `Transaction ${publishedCheckpoint.txid}`;
  } else {
    const go = document.createElement("button");
    go.type = "button";
    go.className = "btn btn-secondary btn-sm";
    go.textContent = "Publish checkpoint";
    go.title = "Anyone may publish it. Opens the approval window; this wallet pays the network fee.";
    go.disabled = preparing;
    go.addEventListener("click", () => void reviewCheckpoint(c));
    v.append(go);
  }
  drow.append(k, v);
  return drow;
}

function renderPayouts(): void {
  el("tr-payouts").hidden = payouts.length === 0 && checkpoint === null && payoutsError === null;
  const rows: HTMLElement[] = [];
  if (checkpoint) rows.push(checkpointRow(checkpoint));
  if (payoutsError !== null) {
    const drow = document.createElement("div");
    drow.className = "drow";
    const k = document.createElement("span");
    k.className = "dkey";
    k.textContent = "Could not read your payouts.";
    k.title = payoutsError;
    drow.append(k);
    rows.push(drow);
  }
  for (const p of payouts) {
    const drow = document.createElement("div");
    drow.className = "drow";
    const k = document.createElement("span");
    k.className = "dkey";
    k.append(depositAmount(p));
    const to = document.createElement("span");
    to.className = "muted";
    to.textContent = p.destination ? ` · to ${shorten(p.destination, 8, 6)}` : " · to the address you withdrew to";
    if (p.destination) to.title = p.destination;
    k.append(to);
    const v = document.createElement("span");
    v.className = "dval";
    const txid = published.get(p.effect_id);
    if (txid) {
      v.textContent = "Published, confirming";
      v.title = `Transaction ${txid}`;
    } else {
      const go = document.createElement("button");
      go.type = "button";
      go.className = "btn btn-primary btn-sm";
      go.textContent = p.destination ? "Publish payout" : "Publish payout…";
      go.title = "Opens the approval window. This wallet pays the network fee.";
      go.disabled = preparing;
      go.addEventListener("click", () => {
        if (p.destination) {
          void reviewPayout(p, p.destination);
        } else {
          askingPayout = p;
          renderPayouts();
          el<HTMLInputElement>("tr-payout-to").focus();
        }
      });
      v.append(go);
    }
    drow.append(k, v);
    rows.push(drow);
  }
  el("tr-payout-rows").replaceChildren(...rows);
  el("tr-payout-ask").hidden = askingPayout === null;
}

function renderDeposits(): void {
  renderPayouts();
  el("tr-deposits").hidden = deposits.length === 0 && depositsError === null;
  const failed: HTMLElement[] = [];
  if (depositsError !== null) {
    const drow = document.createElement("div");
    drow.className = "drow";
    const k = document.createElement("span");
    k.className = "dkey";
    k.textContent = "Could not read your deposits. Do not deposit again until you have checked.";
    k.title = depositsError;
    const v = document.createElement("span");
    v.className = "dval";
    const retry = document.createElement("button");
    retry.type = "button";
    retry.className = "btn btn-secondary btn-sm";
    retry.textContent = "Retry";
    retry.addEventListener("click", () => void loadDeposits());
    v.append(retry);
    drow.append(k, v);
    failed.push(drow);
  }
  el("tr-deposit-rows").replaceChildren(
    ...failed,
    ...deposits.map((d) => {
      const drow = document.createElement("div");
      drow.className = "drow";
      const k = document.createElement("span");
      k.className = "dkey";
      k.append(depositAmount(d));
      const where = document.createElement("span");
      where.className = "mono muted";
      where.textContent = ` · ${shorten(d.txid, 8, 4)}:${d.vout}`;
      k.append(where);
      k.title = `${d.txid}:${d.vout}`;
      if (d.vault_id === null) {
        k.title += "\nThis output could not be read, so its market is not known here. The node refuses to admit it into another market.";
      }
      const v = document.createElement("span");
      v.className = "dval";
      if (d.swept) {
        v.textContent = "Swept in";
        v.title = "The market swept this deposit into its vault: it was admitted.";
      } else if (d.confirmations < DEPOSIT_CONFIRMATIONS) {
        v.textContent = `${Math.max(d.confirmations, 0)} of ${DEPOSIT_CONFIRMATIONS} confirmations`;
      } else {
        const ask = document.createElement("button");
        ask.type = "button";
        ask.className = "btn btn-secondary btn-sm";
        ask.textContent = admitted.has(`${d.txid}:${d.vout}`) ? "Ask again" : "Ask to admit";
        ask.title = "Asks the market to admit this deposit. Nothing is signed and no coins move.";
        ask.disabled = !marketId || !!market.status?.paused;
        ask.addEventListener("click", () => void admit(d, ask));
        v.append(ask);
      }
      drow.append(k, v);
      return drow;
    }),
  );
}

// ---- requests ------------------------------------------------------------------

const REQUESTS_NOTE = el("tr-requests-note").textContent ?? "";

async function loadRequests(): Promise<void> {
  const id = marketId;
  const at = epoch;
  if (!active || !id || requestsLoadingEpoch === at) return;
  requestsLoadingEpoch = at;
  try {
    const list = await invoke<TradeRequest[]>("trade_requests", { marketId: id });
    if (at !== epoch) return;
    requests = list;
    el("tr-requests-note").textContent = REQUESTS_NOTE;
  } catch (e) {
    if (at !== epoch) return;
    el("tr-requests-note").textContent = `Could not read your requests: ${e}`;
  } finally {
    if (requestsLoadingEpoch === at) requestsLoadingEpoch = null;
    if (at === epoch) requestsRead = Date.now();
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
  if (r.kind === "withdraw") return "Withdrawal";
  if (r.kind === "deposit") return "Deposit admission";
  if (r.kind === "other") return "Other request";
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
      return `Refused: ${refusal(o, r.kind)}`;
    case "included_not_executed":
      return "Included, not executed yet";
    case "not_executed":
      return "Not executed";
    default:
      return o.status;
  }
}

function renderRequests(): void {
  const waiting = requests.filter((r) => r.pending).length;
  el("tr-requests-count").textContent = String(waiting);
  el("tr-requests-count").hidden = waiting === 0;
  el("tr-requests-count").title = "Not certified yet";
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
  ["tr-tab-funds", "tr-funds"],
] as const;

function selectTab(tab: (typeof TABS)[number][0]): void {
  for (const [t, p] of TABS) {
    el(t).setAttribute("aria-selected", String(t === tab));
    el(p).hidden = t !== tab;
  }
}
for (const [tab] of TABS) el(tab).addEventListener("click", () => selectTab(tab));

/** The asset card's shortcuts open the Funds tab in that direction. */
for (const [id, way] of [
  ["tr-go-deposit", "in"],
  ["tr-go-withdraw", "out"],
] as const) {
  el(id).addEventListener("click", () => {
    funding = way;
    showFundResult("", "");
    selectTab("tr-tab-funds");
    renderFunding();
    el("tr-funds").scrollIntoView({ behavior: "smooth", block: "nearest" });
    el("tr-fund-amount").focus({ preventScroll: true });
  });
}

for (const [id, view] of [
  ["tr-view-price", "price"],
  ["tr-view-depth", "depth"],
] as const) {
  el(id).addEventListener("click", () => {
    chartView = view;
    renderChart();
  });
}
el("tr-chart-box").addEventListener("mousemove", hoverChart);
el("tr-chart-box").addEventListener("mouseleave", hideCross);

for (const [id, show] of [
  ["tr-show-both", "both"],
  ["tr-show-bids", "bids"],
  ["tr-show-asks", "asks"],
] as const) {
  el(id).addEventListener("click", () => {
    bookShow = show;
    el("tr-book").dataset.show = show;
    for (const b of ["tr-show-both", "tr-show-bids", "tr-show-asks"]) el(b).setAttribute("aria-pressed", String(b === id));
    renderBook();
  });
}

for (const [id, isLadder] of [
  ["tr-type-limit", false],
  ["tr-type-ladder", true],
] as const) {
  el(id).addEventListener("click", () => {
    ladder = isLadder;
    renderForm();
  });
}
for (const button of document.querySelectorAll<HTMLElement>("#tr-ticket .tr-stepper")) {
  button.addEventListener("click", () => nudge(button.dataset.for!, Number(button.dataset.dir)));
}
// The arrow keys step a price or an amount, as the steppers do with a click.
for (const input of document.querySelectorAll<HTMLInputElement>("#tr-ticket .tr-in-field")) {
  input.setAttribute("aria-keyshortcuts", "ArrowUp ArrowDown");
  input.addEventListener("keydown", (e) => {
    if (e.key !== "ArrowUp" && e.key !== "ArrowDown") return;
    e.preventDefault();
    nudge(input.id, e.key === "ArrowUp" ? 1 : -1);
  });
}
el("tr-pct").addEventListener("input", (e) => setShare(Number((e.target as HTMLInputElement).value)));
for (const mark of document.querySelectorAll<HTMLElement>("#tr-slider [data-pct]")) {
  mark.addEventListener("click", () => {
    if (!el<HTMLInputElement>("tr-pct").disabled) setShare(Number(mark.dataset.pct));
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
el("tr-cancel-bid").addEventListener("click", () => void cancelOrder("bid"));
el("tr-cancel-ask").addEventListener("click", () => void cancelOrder("ask"));
el("tr-form").addEventListener("input", renderForm);
for (const [id, way] of [
  ["tr-fund-in", "in"],
  ["tr-fund-out", "out"],
] as const) {
  el(id).addEventListener("click", () => {
    funding = way;
    showFundResult("", "");
    renderFunding();
  });
}
el("tr-fund-form").addEventListener("input", renderFunding);
el("tr-payout-ask").addEventListener("submit", (e) => {
  e.preventDefault();
  const p = askingPayout;
  if (p) void reviewPayout(p, el<HTMLInputElement>("tr-payout-to").value.trim());
});
el("tr-payout-cancel").addEventListener("click", () => {
  askingPayout = null;
  el<HTMLInputElement>("tr-payout-to").value = "";
  renderPayouts();
});
el("tr-payout-pick").addEventListener("click", async () => {
  const picked = await pickAddress("Address you withdrew to", "The payout can only go to the exact address the withdrawal named.", "mine");
  if (picked) el<HTMLInputElement>("tr-payout-to").value = picked.address;
});
el("tr-fund-pick").addEventListener("click", async () => {
  const picked = await pickAddress("Withdraw to", "One of your own addresses, or a saved contact.", "mine");
  if (!picked) return;
  el<HTMLInputElement>("tr-fund-to").value = picked.address;
  renderFunding();
});
el("tr-fund-asset").addEventListener("change", renderFunding);
el("tr-fund-form").addEventListener("submit", (e) => {
  e.preventDefault();
  void reviewFunding();
});
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
  if (active && marketId && Date.now() - depositsRead > REQUESTS_IDLE_MS) void loadDeposits();
}, 1000);

/** Re-render after amounts were masked or unmasked. */
export function tradeMaskChanged(): void {
  if (active) render();
}
