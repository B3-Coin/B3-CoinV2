// The Trade page: one market of a FlowMesh V2 network (regtest), kept live
// while the page is open.
//
// What is shown comes in two kinds, and the page says which is which:
// - certified: this account's balances and the result of each action it
//   signed. The core checked their proofs against the genesis document
//   before sending them here.
// - a seat's report: the order book and the trades, from one seat's live
//   feed, which carries no proof.
//
// This page never signs. An order, a cancellation or a transfer is drafted
// by the core, which checks it again, and is signed only when it is
// approved in the approval window.
import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";

import { bannerTone, el, isMasked } from "../shared/ui";
import {
  amount,
  amountText,
  baseAtoms,
  depth,
  feeRate,
  lotText,
  maxLots,
  outcomeText,
  parseAmount,
  parsePrice,
  parseWhole,
  percent,
  price,
  quoteAtoms,
  sellerFee,
  shortId,
  spread,
  tickText,
  tifText,
  unit,
  whole,
  type Asset,
  type Level,
  type Market,
} from "./fm2fmt";

// ---- what the core sends (src-tauri/src/fm2/view.rs, src/trade.rs) ---------

type Info = {
  chain: string;
  profile: string;
  domain: string;
  seat_set: string;
  quorum: number;
  seats: { seat: number; address: string }[];
  assets: Asset[];
  markets: Market[];
  account: string | null;
  fee_ppm: number;
  max_open_orders: number;
  window_slots: number;
  recovered_bytes: number;
};

type Amount = { asset: Asset; atoms: string };

type Intent =
  | { action: "place"; market: Market; side: "buy" | "sell"; tif: string; price_ticks: string; lots: string; base: Amount; quote: Amount }
  | { action: "cancel"; order: string; placed: Intent | null }
  | { action: "cancel_all" }
  | { action: "transfer"; to: string; amount: Amount }
  | { action: "other"; kind: number };

type Fill = { market: string; side: "buy" | "sell"; price_ticks: string; lots: string; quote: Amount; fee_atoms: string | null };

type Row = {
  label: string;
  created: number;
  action_id: string;
  nonce: string;
  ref_slot: string;
  window_slots: number;
  intent: Intent;
} & (
  | { state: "open"; note: string }
  | {
      state: "included";
      height: string;
      index: number;
      code: number;
      code_name: string;
      accepted: boolean;
      remaining_lots: string;
      fills: Fill[];
      deltas: { asset: Asset; available: string; reserved: string }[];
    }
  | { state: "expired"; height: string }
);

type Order = {
  order: string;
  label: string;
  market: string;
  market_id: string;
  side: "buy" | "sell";
  price_ticks: string;
  placed_lots: string;
  rested_lots: string;
  seen_filled_lots: string;
  remaining_lots: string;
  height: string;
  cancelling: boolean;
};

type AccountView = {
  account: string | null;
  seats: { seat: number; height: string | null; proved: boolean; error: string | null }[];
  height: string | null;
  proved: boolean;
  balances: { asset: Asset; available: string; reserved: string }[];
  open_orders: number | null;
  orders: Order[];
  orders_mismatch: string | null;
  activity: Row[];
};

type Health = { seat: number; reachable: boolean; fenced: boolean; published_height: string | null; pool_live: string | null; error: string | null };

type Admission = { seat: number; code: string; reason: string; accepted: boolean };
type Sent = { label: string; action_id: string; nonce: string; ref_slot: string; window_slots: number; admissions: Admission[]; accepted: number };

/** A draft's end, as the main window receives it ("draft-result"). */
export type TradeOutcome = { outcome: "fm2"; id: string; sent: Sent };
type Decided = { outcome: string; id: string; error?: string; sent?: Sent };

type Trade = { at: number; slot: string; seat: number; takerSide: "buy" | "sell"; ticks: bigint; lots: bigint; quote: bigint; mine: "maker" | "taker" | null };

// ---- state -----------------------------------------------------------------

/** The newest trades kept for the tape and the chart. */
const TRADES_KEPT = 300;
/** Book levels shown per side when both sides show. */
const BOOK_ROWS = 12;
/** A row that is new in the book flashes for this long. */
const FLASH_MS = 800;
/** Seats are asked for their status this often while the page is open. */
const HEALTH_MS = 5000;
const MASK = "••••";

let active = false;
let wired = false;
let info: Info | null = null;
let market: Market | null = null;
let book: { bids: Level[]; asks: Level[]; slot: string; seat: number } | null = null;
let trades: Trade[] = [];
let account: AccountView | null = null;
let health: Health[] = [];
let healthRead = 0;
let slot: { height: string; seat: number } | null = null;
/** Whether each feed is live, and on which seat. */
const links = new Map<string, { seat: number; live: boolean; detail: string }>();

let side: "buy" | "sell" = "buy";
let tif: "gtc" | "ioc" | "post_only" = "gtc";
let bookShow: "both" | "bids" | "asks" = "both";
let chartView: "price" | "depth" = "price";
const shown = { asks: new Set<string>(), bids: new Set<string>() };

/** Drafts this page asked for and the approval window has not decided. */
const drafts = new Map<string, "order" | "cancel" | "transfer">();
/** Signed actions whose certified end the page still reports to the user. */
const awaited = new Map<string, "order" | "cancel" | "transfer">();

const masked = (text: string): string => (isMasked() ? MASK : text);
/** A number as an input holds it: without the digit grouping. */
const plain = (text: string): string => text.replace(/\s/g, "");

function cell(text: string, cls = ""): HTMLTableCellElement {
  const td = document.createElement("td");
  td.className = cls;
  td.textContent = text;
  return td;
}

function span(text: string, cls = ""): HTMLSpanElement {
  const node = document.createElement("span");
  node.className = cls;
  node.textContent = text;
  return node;
}

function badge(text: string, tone: string, title = ""): HTMLSpanElement {
  const node = span(text, `badge ${tone ? `badge-${tone}` : ""}`);
  if (title) node.title = title;
  return node;
}

function banner(id: string, tone: string, text: string): void {
  const box = el(id);
  box.hidden = text === "";
  box.textContent = text;
  if (text) bannerTone(box, tone, box.classList.contains("tr-result") ? "tr-result" : "");
}

// ---- entering and leaving --------------------------------------------------

export function startTrade(): void {
  active = true;
  void enter();
}

export function stopTrade(): void {
  if (!active) return;
  active = false;
  invoke("fm2_unwatch").catch(() => {});
}

/** Re-render after amounts were masked or unmasked. */
export function tradeMaskChanged(): void {
  if (active && info) render();
}

async function enter(): Promise<void> {
  if (!wired) {
    wired = true;
    await wire();
  }
  try {
    info = await invoke<Info | null>("fm2_info");
  } catch (e) {
    info = null;
    banner("tr-error", "danger", String(e));
  }
  if (!active) return;
  if (info) await opened();
  else await showConnect();
}

async function showConnect(): Promise<void> {
  for (const id of ["tr-head", "tr-body", "tr-nokey"]) el(id).hidden = true;
  el("tr-connect").hidden = false;
  const genesis = el<HTMLInputElement>("tr-genesis");
  const pins = el<HTMLInputElement>("tr-pins");
  if (genesis.value === "" && pins.value === "") {
    const last = await invoke<{ genesis: string; pins: string } | null>("fm2_last").catch(() => null);
    if (last) {
      genesis.value = last.genesis;
      pins.value = last.pins;
    }
  }
}

async function connect(): Promise<void> {
  const button = el<HTMLButtonElement>("tr-connect-go");
  banner("tr-connect-error", "danger", "");
  button.disabled = true;
  try {
    info = await invoke<Info>("fm2_connect", {
      genesis: el<HTMLInputElement>("tr-genesis").value,
      pins: el<HTMLInputElement>("tr-pins").value,
    });
    await opened();
  } catch (e) {
    banner("tr-connect-error", "danger", String(e));
  } finally {
    button.disabled = false;
  }
}

async function disconnect(): Promise<void> {
  await invoke("fm2_disconnect").catch(() => {});
  info = null;
  market = null;
  account = null;
  book = null;
  trades = [];
  health = [];
  slot = null;
  links.clear();
  drafts.clear();
  awaited.clear();
  banner("tr-error", "danger", "");
  await showConnect();
}

/** A connection is there: show its markets and follow one. */
async function opened(): Promise<void> {
  if (!info) return;
  banner("tr-error", "danger", "");
  el("tr-connect").hidden = true;
  el("tr-head").hidden = false;
  el("tr-body").hidden = false;
  const picker = el<HTMLSelectElement>("tr-market");
  picker.replaceChildren(
    ...info.markets.map((m) => {
      const option = document.createElement("option");
      option.value = m.id;
      option.textContent = m.name;
      return option;
    }),
  );
  const assets = el<HTMLSelectElement>("tr-transfer-asset");
  assets.replaceChildren(
    ...info.assets.map((a) => {
      const option = document.createElement("option");
      option.value = a.id;
      option.textContent = a.native ? "B3" : a.name;
      return option;
    }),
  );
  if (info.markets.length === 0) {
    banner("tr-error", "warning", "This network's genesis document lists no market.");
    el("tr-body").hidden = true;
    return;
  }
  const keep = info.markets.find((m) => m.id === market?.id) ?? info.markets[0];
  picker.value = keep.id;
  if (info.recovered_bytes > 0) {
    banner("tr-error", "warning", "B3 Hive's record of signed actions ended in a half-written entry, from a crash while it was being saved. That entry was dropped; it had not been sent.");
  }
  await selectMarket(keep.id);
}

async function selectMarket(id: string): Promise<void> {
  if (!info) return;
  market = info.markets.find((m) => m.id === id) ?? null;
  book = null;
  trades = [];
  shown.asks.clear();
  shown.bids.clear();
  links.clear();
  clearTicket();
  render();
  if (!market) return;
  try {
    await invoke("fm2_watch", { market: market.id });
  } catch (e) {
    banner("tr-error", "danger", String(e));
  }
  void refreshAccount();
  void refreshHealth();
}

// ---- reads -----------------------------------------------------------------

let accountBusy = false;
let accountAgain = false;

/** Reads the proved account and the outbox. Calls made meanwhile collapse
 *  into one more read. */
async function refreshAccount(): Promise<void> {
  if (!info) return;
  if (accountBusy) {
    accountAgain = true;
    return;
  }
  accountBusy = true;
  try {
    account = await invoke<AccountView>("fm2_account");
    if (info && account.account && !info.account) info.account = account.account;
    reportEnds();
    if (active) renderAccount();
  } catch (e) {
    if (active && info) banner("tr-error", "danger", String(e));
  } finally {
    accountBusy = false;
    if (accountAgain) {
      accountAgain = false;
      void refreshAccount();
    }
  }
}

async function refreshHealth(): Promise<void> {
  if (!info) return;
  healthRead = Date.now();
  try {
    health = await invoke<Health[]>("fm2_health");
  } catch {
    health = [];
  }
  if (active && info) renderHead();
}

// ---- rendering -------------------------------------------------------------

function render(): void {
  renderHead();
  renderBook();
  renderChart();
  renderTrades();
  renderAccount();
}

function renderHead(): void {
  if (!info) return;
  const m = market;
  el("tr-lot").textContent = m ? lotText(m) : "—";
  el("tr-tick").textContent = m ? tickText(m) : "—";
  el("tr-slot").textContent = slot ? whole(BigInt(slot.height)) : "—";
  const up = health.filter((h) => h.reachable).length;
  el("tr-quorum").textContent = `${health.length ? up : "—"} of ${info.seats.length} / ${info.quorum}`;
  const bids = book?.bids ?? [];
  const asks = book?.asks ?? [];
  el("tr-best-bid").textContent = m && bids.length ? price(BigInt(bids[0].price_ticks), m) : "—";
  el("tr-best-ask").textContent = m && asks.length ? price(BigInt(asks[0].price_ticks), m) : "—";

  const last = trades[0];
  const before = trades.find((t) => t.ticks !== last?.ticks);
  el("tr-last").textContent = m && last ? price(last.ticks, m) : "—";
  el("tr-last").className = `tr-last tnum ${last && before ? (last.ticks > before.ticks ? "tr-up" : "tr-down") : ""}`;
  el("tr-change").textContent = m && last ? `${unit(m.quote)} per lot` : m ? "No trade seen yet" : "";

  const slots = links.get("slots");
  const live = el("tr-live");
  live.textContent = slots?.live ? "Live" : "Connecting";
  live.className = `badge ${slots?.live ? "badge-success" : "badge-warning"}`;
  live.title = slots?.live ? `Certified slots are arriving from seat ${slots.seat}.` : slots?.detail || "Waiting for a seat's feed.";

  el("tr-seats").replaceChildren(
    ...info.seats.map((s) => {
      const h = health.find((x) => x.seat === s.seat);
      const text = !h ? `Seat ${s.seat}` : h.reachable ? `Seat ${s.seat} · slot ${h.published_height ?? "—"}` : `Seat ${s.seat} · no answer`;
      const tone = !h ? "" : !h.reachable ? "danger" : h.fenced ? "warning" : "success";
      const title = !h
        ? s.address
        : h.reachable
          ? `${s.address}\nThe seat's own report, not certified.${h.fenced ? "\nThis seat says it has stopped taking part (fenced)." : ""}\nActions waiting in its pool: ${h.pool_live ?? "—"}`
          : `${s.address}\n${h.error ?? ""}`;
      return badge(text, tone, title);
    }),
  );
  el("tr-source").textContent =
    `Network ${shortId(info.domain)} · rules ${info.profile} · every connection is pinned to its seat's key, and certificates are checked against the ${info.seats.length} seats of the genesis document.`;
  el("tr-source").title = `Domain ${info.domain}\nSeat set ${info.seat_set}`;
}

function renderBook(): void {
  const m = market;
  const asks = el("tr-asks");
  const bids = el("tr-bids");
  const link = links.get("book");
  el("tr-book-stale").hidden = !!link?.live;
  el("tr-book-price").title = m ? `In ${unit(m.quote)}, for one lot` : "";
  el("tr-book-note").textContent = book
    ? `As seat ${book.seat} reports it after slot ${whole(BigInt(book.slot))}. A seat's report, not certified. Choose a level to use its price.`
    : "Waiting for a seat's book feed.";
  if (!m || !book) {
    asks.replaceChildren();
    bids.replaceChildren();
    el("tr-mid-last").textContent = "—";
    el("tr-spread").textContent = "—";
    return;
  }
  const rows = bookShow === "both" ? BOOK_ROWS : 2 * BOOK_ROWS;
  const askLevels = depth(book.asks).slice(0, rows);
  const bidLevels = depth(book.bids).slice(0, rows);
  const own = new Set((account?.orders ?? []).filter((o) => o.market_id === m.id).map((o) => `${o.side === "sell" ? "asks" : "bids"}:${o.price_ticks}`));
  // One depth scale for both sides.
  const deepest = [...askLevels, ...bidLevels].reduce((max, l) => (l.total > max ? l.total : max), 0n);
  const row = (level: (typeof askLevels)[number], which: "asks" | "bids", previous: Set<string>) => {
    const key = level.ticks.toString();
    // A button, so the keyboard can take a level's price too.
    const button = document.createElement("button");
    button.type = "button";
    button.dataset.level = `${which}:${key}`;
    button.className = `tr-book-row tr-pick tr-${which === "asks" ? "ask" : "bid"}`;
    const mine = own.has(`${which}:${key}`);
    if (mine) button.classList.add("tr-own");
    if (previous.size && !previous.has(key)) {
      button.classList.add("tr-flash");
      setTimeout(() => button.classList.remove("tr-flash"), FLASH_MS);
    }
    const share = deepest > 0n ? Number((level.total * 1000n) / deepest) / 10 : 0;
    button.style.setProperty("--depth", `${share}%`);
    button.append(span(price(level.ticks, m), "tr-price"), span(whole(level.lots), "num"), span(whole(level.total), "num"));
    button.title = `${level.orders} order${level.orders === 1 ? "" : "s"} · ${amountText(baseAtoms(level.lots, m), m.base)}${mine ? "\nAn order of yours rests here" : ""}\nClick to use this price`;
    button.setAttribute(
      "aria-label",
      `${which === "asks" ? "Ask" : "Bid"} ${price(level.ticks, m)} ${unit(m.quote)}, ${level.lots} lots${mine ? ", includes yours" : ""}. Use this price.`,
    );
    button.addEventListener("click", () => pickPrice(level.ticks));
    return button;
  };
  // Redrawing must not take the keyboard focus off the level it was on.
  const focused = (document.activeElement as HTMLElement | null)?.dataset.level;
  // Asks above the spread, best (lowest) nearest it.
  asks.replaceChildren(...[...askLevels].reverse().map((l) => row(l, "asks", shown.asks)));
  bids.replaceChildren(...bidLevels.map((l) => row(l, "bids", shown.bids)));
  if (focused) el("tr-book").querySelector<HTMLElement>(`[data-level="${focused}"]`)?.focus();
  shown.asks = new Set(askLevels.map((l) => l.ticks.toString()));
  shown.bids = new Set(bidLevels.map((l) => l.ticks.toString()));

  el("tr-mid-last").textContent = trades.length ? price(trades[0].ticks, m) : "—";
  const gap = spread(book.bids, book.asks);
  el("tr-spread").textContent = gap
    ? `Spread ${price(gap.ticks, m)} · ${percent(gap.bps)}`
    : book.bids.length && book.asks.length
      ? "The best bid and ask meet"
      : book.bids.length || book.asks.length
        ? "One-sided book"
        : "No resting orders";
}

const SVG = "http://www.w3.org/2000/svg";
const [CHART_W, CHART_H, CHART_PAD] = [600, 260, 14];
/** Prices take the top of the chart; the lots traded the bottom. */
const PRICE_BOTTOM = CHART_H * 0.76;
/** Room on the right for the last-price tag. */
const TAG_ROOM = 64;

function svg(tag: string, attrs: Record<string, string>): SVGElement {
  const node = document.createElementNS(SVG, tag);
  for (const [k, v] of Object.entries(attrs)) node.setAttribute(k, v);
  return node;
}

function renderChart(): void {
  const showPrice = chartView === "price";
  // SVG elements have no hidden property: set the attribute.
  el("tr-chart").toggleAttribute("hidden", !showPrice);
  el("tr-depth").toggleAttribute("hidden", showPrice);
  el("tr-view-price").setAttribute("aria-selected", String(showPrice));
  el("tr-view-depth").setAttribute("aria-selected", String(!showPrice));
  const caption = el("tr-chart-caption");
  caption.textContent = showPrice ? "Trades seen since this page connected" : "Resting orders by price";
  caption.title = showPrice
    ? "Each point is one fill a seat reported, in the order they arrived. FlowMesh V2 serves no price history, so the chart starts when this page connects. A seat's report, not certified."
    : "The running total of lots resting at each price, as a seat reports the book. Not certified.";
  for (const id of ["tr-chart-high", "tr-chart-low", "tr-chart-first", "tr-chart-mid", "tr-chart-last"]) el(id).textContent = "";
  el("tr-lasttag").hidden = true;
  if (showPrice) renderPriceChart();
  else renderDepthChart();
}

function renderPriceChart(): void {
  const chart = el("tr-chart");
  const m = market;
  const empty = el("tr-chart-empty");
  // Oldest first.
  const points = [...trades].reverse();
  empty.textContent = "No trade has been seen since this page connected.";
  empty.hidden = points.length > 0;
  if (!m || !points.length) {
    chart.replaceChildren();
    chart.setAttribute("aria-label", "Trade prices: none seen yet");
    return;
  }
  const [W, H, pad] = [CHART_W, CHART_H, CHART_PAD];
  const prices = points.map((t) => t.ticks);
  const hi = prices.reduce((a, b) => (b > a ? b : a));
  const lo = prices.reduce((a, b) => (b < a ? b : a));
  // Plot positions only; every label is formatted exactly from the BigInt.
  const range = Number(hi - lo) || 1;
  const right = W - TAG_ROOM;
  const x = (i: number) => (points.length === 1 ? right / 2 : (i / (points.length - 1)) * (right - 2 * pad) + pad);
  const y = (p: bigint) => (hi === lo ? PRICE_BOTTOM / 2 : PRICE_BOTTOM - pad - (Number(p - lo) / range) * (PRICE_BOTTOM - 2 * pad));

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
  // A step line: the price holds until the next trade, and to now.
  const path: string[] = [];
  points.forEach((_, i) => {
    if (i > 0) path.push(`${x(i).toFixed(1)},${y(prices[i - 1]).toFixed(1)}`);
    path.push(`${x(i).toFixed(1)},${y(prices[i]).toFixed(1)}`);
  });
  const lastY = y(prices[prices.length - 1]);
  path.push(`${right.toFixed(1)},${lastY.toFixed(1)}`);
  const area = svg("polygon", { fill: "url(#tr-fill)", points: `${x(0).toFixed(1)},${PRICE_BOTTOM} ${path.join(" ")} ${right.toFixed(1)},${PRICE_BOTTOM}` });
  const line = svg("polyline", { class: "line", points: path.join(" ") });
  const lastLine = svg("line", { class: "lastline", x1: "0", x2: String(W), y1: lastY.toFixed(1), y2: lastY.toFixed(1) });
  // Lots traded, on their own scale below the prices.
  const maxLotsTraded = points.reduce((a, t) => (t.lots > a ? t.lots : a), 0n);
  const gapX = points.length === 1 ? 12 : ((right - 2 * pad) / (points.length - 1)) * 0.6;
  const barW = Math.max(2, Math.min(12, gapX));
  const room = H - PRICE_BOTTOM - 4;
  const bars = points.map((t, i) => {
    const h = maxLotsTraded > 0n ? (Number((t.lots * 1000n) / maxLotsTraded) / 1000) * room : 0;
    return svg("rect", { class: "vol", x: (x(i) - barW / 2).toFixed(1), y: (H - h).toFixed(1), width: barW.toFixed(1), height: Math.max(1, h).toFixed(1) });
  });
  const dots = points.length <= 60 ? points.map((_, i) => svg("circle", { cx: x(i).toFixed(1), cy: y(prices[i]).toFixed(1), r: "2.5", fill: "var(--accent)" })) : [];
  chart.replaceChildren(defs, ...grid, ...bars, area, line, lastLine, ...dots);

  el("tr-chart-high").textContent = price(hi, m);
  el("tr-chart-low").textContent = hi === lo ? "" : price(lo, m);
  const tag = el("tr-lasttag");
  tag.textContent = price(prices[prices.length - 1], m);
  tag.style.top = `${((lastY / H) * 100).toFixed(2)}%`;
  tag.hidden = false;
  el("tr-chart-first").textContent = `Slot ${whole(BigInt(points[0].slot))}`;
  el("tr-chart-mid").textContent = `${points.length} trade${points.length === 1 ? "" : "s"}`;
  el("tr-chart-last").textContent = `Slot ${whole(BigInt(points[points.length - 1].slot))}`;
  // The eye reads the line; this says the whole chart at once.
  chart.setAttribute(
    "aria-label",
    `Trade prices in ${unit(m.quote)} per lot: ${points.length} trade${points.length === 1 ? "" : "s"}, last ${price(prices[prices.length - 1], m)}, high ${price(hi, m)}, low ${price(lo, m)}`,
  );
}

function renderDepthChart(): void {
  const chart = el("tr-depth");
  const m = market;
  const b = depth(book?.bids ?? []);
  const a = depth(book?.asks ?? []);
  const empty = el("tr-chart-empty");
  empty.textContent = book ? "No resting orders." : "Waiting for a seat's book feed.";
  empty.hidden = !!m && (b.length > 0 || a.length > 0);
  if (!m || (!b.length && !a.length)) {
    chart.replaceChildren();
    chart.setAttribute("aria-label", "Book depth: no resting orders");
    return;
  }
  chart.setAttribute(
    "aria-label",
    `Book depth: ${b.length} bid level${b.length === 1 ? "" : "s"}${b.length ? `, best ${price(b[0].ticks, m)}` : ""}; ` +
      `${a.length} ask level${a.length === 1 ? "" : "s"}${a.length ? `, best ${price(a[0].ticks, m)}` : ""}`,
  );
  const [W, H, pad] = [CHART_W, CHART_H, CHART_PAD];
  const all = [...b, ...a];
  let pLo = all.reduce((v, l) => (l.ticks < v ? l.ticks : v), all[0].ticks);
  let pHi = all.reduce((v, l) => (l.ticks > v ? l.ticks : v), all[0].ticks);
  if (pLo === pHi) {
    pLo = pLo > 10n ? pLo - 10n : 0n;
    pHi = pHi + 10n;
  }
  const cMax = all.reduce((v, l) => (l.total > v ? l.total : v), 1n);
  const x = (p: bigint) => pad + (Number(p - pLo) / Number(pHi - pLo)) * (W - 2 * pad);
  const y = (c: bigint) => H - pad - (Number((c * 1000n) / cMax) / 1000) * (H - 2 * pad);
  const bottom = (H - pad).toFixed(1);
  // Levels come best first: a step outwards from the spread.
  const path = (levels: typeof all, end: bigint): string => {
    const pts: string[] = [`${x(levels[0].ticks).toFixed(1)},${bottom}`];
    levels.forEach((l, i) => {
      if (i > 0) pts.push(`${x(l.ticks).toFixed(1)},${y(levels[i - 1].total).toFixed(1)}`);
      pts.push(`${x(l.ticks).toFixed(1)},${y(l.total).toFixed(1)}`);
    });
    pts.push(`${x(end).toFixed(1)},${y(levels[levels.length - 1].total).toFixed(1)}`);
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
  el("tr-chart-high").textContent = `${whole(cMax)} lots`;
  el("tr-chart-first").textContent = price(pLo, m);
  el("tr-chart-mid").textContent = `${b.length} bid and ${a.length} ask level${a.length === 1 ? "" : "s"}`;
  el("tr-chart-last").textContent = price(pHi, m);
}

function clock(ms: number): string {
  const d = new Date(ms);
  const two = (n: number) => String(n).padStart(2, "0");
  return `${two(d.getHours())}:${two(d.getMinutes())}:${two(d.getSeconds())}`;
}

function renderTrades(): void {
  const m = market;
  const body = el("tr-trades-rows");
  const link = links.get("trades");
  el("tr-trades-note").textContent = !m
    ? ""
    : trades.length
      ? `Fills as a seat reported them since this page connected, newest first. A seat's report, not certified. Prices are in ${unit(m.quote)} per lot.`
      : link?.live
        ? "No trade has happened in this market since this page connected. FlowMesh V2 serves no trade history."
        : "Waiting for a seat's trade feed.";
  if (!m) {
    body.replaceChildren();
    return;
  }
  body.replaceChildren(
    ...trades.map((t) => {
      const tr = document.createElement("tr");
      const taker = cell(t.takerSide === "buy" ? "Buy" : "Sell", t.takerSide === "buy" ? "tr-up" : "tr-down");
      const yours = cell(t.mine === "maker" ? "Your resting order" : t.mine === "taker" ? "Your order" : "");
      tr.append(cell(clock(t.at), "tnum"), taker, cell(price(t.ticks, m), "num tnum"), cell(whole(t.lots), "num tnum"), cell(amount(t.quote, m.quote), "num tnum"), yours, cell(whole(BigInt(t.slot)), "num tnum"));
      return tr;
    }),
  );
}

function describe(intent: Intent): string {
  switch (intent.action) {
    case "place": {
      const m = intent.market;
      const lots = BigInt(intent.lots);
      return `${intent.side === "buy" ? "Buy" : "Sell"} ${whole(lots)} lot${lots === 1n ? "" : "s"} of ${m.name} at ${price(BigInt(intent.price_ticks), m)} ${unit(m.quote)} · ${tifText(intent.tif)}`;
    }
    case "cancel":
      return intent.placed ? `Cancel: ${describe(intent.placed)}` : `Cancel order ${shortId(intent.order)}`;
    case "cancel_all":
      return "Cancel all orders";
    case "transfer":
      return `Transfer ${amountText(BigInt(intent.amount.atoms), intent.amount.asset)} to ${shortId(intent.to)}`;
    default:
      return `An action of kind ${intent.kind}`;
  }
}

/** What a certified outcome did, in one line. */
function resultText(row: Extract<Row, { state: "included" }>): string {
  const parts = [outcomeText(row.code_name)];
  if (row.fills.length) {
    const lots = row.fills.reduce((sum, f) => sum + BigInt(f.lots), 0n);
    const quote = row.fills.reduce((sum, f) => sum + BigInt(f.quote.atoms), 0n);
    const fee = row.fills.reduce((sum, f) => sum + BigInt(f.fee_atoms ?? "0"), 0n);
    const unitText = unit(row.fills[0].quote.asset);
    parts.push(
      `${whole(lots)} lot${lots === 1n ? "" : "s"} ${row.fills[0].side === "buy" ? "bought" : "sold"} for ${amount(quote, row.fills[0].quote.asset)} ${unitText} in ${row.fills.length} fill${row.fills.length === 1 ? "" : "s"}` +
        (fee > 0n ? `, fee ${amount(fee, row.fills[0].quote.asset)} ${unitText}` : ""),
    );
  }
  if (row.intent.action === "place" && BigInt(row.remaining_lots) > 0n) parts.push(`${whole(BigInt(row.remaining_lots))} lots resting`);
  return parts.join(" · ");
}

function renderAccount(): void {
  if (!info) return;
  const hasKey = info.account !== null;
  el("tr-nokey").hidden = hasKey;
  const a = account;

  // The account card.
  el("tr-account-id").textContent = info.account ?? "No account yet";
  el("tr-account-id").title = info.account ?? "";
  el<HTMLButtonElement>("tr-account-copy").hidden = !hasKey;
  const answered = a ? a.seats.filter((s) => s.height !== null).length : 0;
  const proving = a ? a.seats.filter((s) => s.proved).length : 0;
  el("tr-account-proof").textContent = !hasKey
    ? ""
    : !a
      ? "Reading"
      : a.proved
        ? `Certified at slot ${whole(BigInt(a.height ?? "0"))} · ${proving} of ${a.seats.length} seats`
        : answered
          ? "Not on the network yet"
          : "No seat answered";
  el("tr-account-proof").title = a ? a.seats.map((s) => `Seat ${s.seat}: ${s.error ?? (s.proved ? `proved at slot ${s.height}` : `does not know the account (slot ${s.height})`)}`).join("\n") : "";
  const note = el("tr-account-note");
  note.hidden = !hasKey || !a || a.proved;
  note.textContent = !a || a.proved
    ? ""
    : answered
      ? "The seats do not know this account yet. It appears with its first deposit; see below."
      : "No seat answered with a certificate that checks out, so no balance is shown.";
  el("tr-balances").replaceChildren(
    ...(a?.proved ? a.balances : []).map((b) => {
      const row = document.createElement("div");
      row.className = "tr-asset";
      const held = span("", "tr-asset-held t-xs muted");
      held.append(span("In orders"), span(masked(amountText(BigInt(b.reserved), b.asset)), "tnum"));
      row.append(span(b.asset.native ? "B3" : b.asset.name, "tr-asset-name"), span(masked(amountText(BigInt(b.available), b.asset)), "tr-asset-free tnum"), held);
      return row;
    }),
  );
  el("tr-faucet").textContent = `fm2-devchain --file <devchain log> --genesis <genesis.json> deposit ${info.account ?? "<account>"} <asset> <atoms>`;

  // Open orders.
  const orders = a?.orders ?? [];
  const count = el("tr-orders-count");
  count.hidden = orders.length === 0;
  count.textContent = String(orders.length);
  el("tr-orders-actions").hidden = orders.length === 0;
  const mismatch = el("tr-orders-mismatch");
  mismatch.hidden = !a?.orders_mismatch;
  mismatch.textContent = a?.orders_mismatch
    ? `${a.orders_mismatch} This list may be out of date: an order can have filled while B3 Hive was not watching. The balances beside it are certified and correct.`
    : "";
  el("tr-orders-note").textContent = !hasKey
    ? "Create an account to place orders."
    : orders.length
      ? "Rebuilt from this account's certified results and the fills a seat reported since. FlowMesh V2 cannot list an account's orders yet, so B3 Hive checks this list against the certified account: its count of open orders and what it holds in reserve."
      : a && a.open_orders !== null && a.open_orders > 0
        ? ""
        : "No open orders.";
  el("tr-orders-rows").replaceChildren(
    ...orders.map((o) => {
      const m = info?.markets.find((x) => x.id === o.market_id);
      const tr = document.createElement("tr");
      const left = BigInt(o.remaining_lots);
      const ticks = BigInt(o.price_ticks);
      const action = document.createElement("td");
      const button = document.createElement("button");
      button.type = "button";
      button.className = "btn btn-secondary btn-sm";
      button.textContent = o.cancelling ? "Cancelling" : "Cancel";
      button.disabled = o.cancelling;
      button.title = o.cancelling ? "A cancellation is signed and waits for a certified slot. Until then the order can still fill." : "";
      button.addEventListener("click", () => void cancelOrder(o.order, button));
      action.append(button);
      tr.append(
        cell(o.market),
        cell(o.side === "buy" ? "Buy" : "Sell", o.side === "buy" ? "tr-up" : "tr-down"),
        cell(m ? `${price(ticks, m)} ${unit(m.quote)}` : o.price_ticks, "num tnum"),
        cell(`${whole(left)} of ${whole(BigInt(o.placed_lots))}`, "num tnum"),
        cell(m ? masked(amountText(quoteAtoms(left, ticks, m), m.quote)) : "—", "num tnum"),
        cell(whole(BigInt(o.height)), "num tnum"),
        action,
      );
      return tr;
    }),
  );

  // Activity.
  const rows = a?.activity ?? [];
  const waiting = rows.filter((r) => r.state === "open").length;
  const badgeNode = el("tr-activity-count");
  badgeNode.hidden = waiting === 0;
  badgeNode.textContent = String(waiting);
  badgeNode.title = `${waiting} signed action${waiting === 1 ? "" : "s"} without a certified result yet`;
  el("tr-activity-rows").replaceChildren(
    ...rows.map((r) => {
      const tr = document.createElement("tr");
      const status = document.createElement("td");
      const result = document.createElement("td");
      const action = document.createElement("td");
      if (r.state === "open") {
        status.append(badge("Waiting", "warning", "Signed and stored. No certified slot holds it yet."));
        result.textContent = r.note || `Valid for ${r.window_slots} slots after slot ${r.ref_slot}.`;
        const again = document.createElement("button");
        again.type = "button";
        again.className = "btn btn-secondary btn-sm";
        again.textContent = "Post again";
        again.title = "Sends the same signed action to every seat again. Nothing new is signed; a seat that already has it says so.";
        again.addEventListener("click", () => void postAgain(r.label, again, result));
        action.append(again);
      } else if (r.state === "expired") {
        status.append(badge("Expired", "", "The seats proved that no slot can include it any more. Nothing happened."));
        result.textContent = `Proved at slot ${whole(BigInt(r.height))}. It never took effect.`;
      } else {
        status.append(r.accepted ? badge("Certified", "success", `In slot ${r.height}, checked against the genesis seats.`) : badge("Rejected", "danger", `The network's rules refused it in slot ${r.height}. Certified.`));
        result.textContent = `${resultText(r)} · slot ${whole(BigInt(r.height))}`;
      }
      const what = cell(describe(r.intent));
      what.title = `Action ${r.action_id}\n${r.label}`;
      tr.append(cell(new Date(r.created * 1000).toLocaleString(), "tnum"), what, cell(r.nonce, "num tnum"), status, result, action);
      return tr;
    }),
  );

  renderTicket();
  renderTransfer();
  if (book) renderBook();
}

// ---- the order ticket ------------------------------------------------------

/** What this account can spend on the ticket's side; null when no seat has
 *  proved the account. */
function available(asset: Asset): bigint | null {
  if (!account?.proved) return null;
  const held = account.balances.find((b) => b.asset.id === asset.id);
  return held ? BigInt(held.available) : 0n;
}

function gate(): string | null {
  if (!info?.account) return "Create an account to place orders.";
  if (!account) return "Reading the account.";
  if (!account.proved) return "This account has no balance on the network yet. See Deposit and withdraw.";
  return null;
}

function clearTicket(): void {
  for (const id of ["tr-price", "tr-lots"]) el<HTMLInputElement>(id).value = "";
  banner("tr-result", "", "");
}

function renderTicket(): void {
  const m = market;
  if (!m || !info) return;
  const buy = side === "buy";
  el("tr-ticket").dataset.side = buy ? "bid" : "ask";
  el("tr-buy").setAttribute("aria-selected", String(buy));
  el("tr-sell").setAttribute("aria-selected", String(!buy));
  for (const t of ["gtc", "ioc", "post_only"] as const) el(`tr-type-${t}`).setAttribute("aria-selected", String(t === tif));
  el("tr-price-unit").textContent = unit(m.quote);
  const spend = buy ? m.quote : m.base;
  const have = available(spend);
  el("tr-avbl").textContent = have === null ? "—" : masked(amountText(have, spend));

  const priceText = el<HTMLInputElement>("tr-price").value.trim();
  const lotsText = el<HTMLInputElement>("tr-lots").value.trim();
  const ticks = priceText === "" ? null : parsePrice(priceText, m);
  const lots = lotsText === "" ? null : parseWhole(lotsText);
  let error = "";
  if (priceText !== "" && ticks === null) error = `The price must be a whole number of ticks above zero. One tick is ${tickText(m)}.`;
  else if (lotsText !== "" && (lots === null || lots === 0n)) error = `The amount is a whole number of lots. One lot is ${lotText(m)}.`;

  const base = lots !== null ? baseAtoms(lots, m) : null;
  const quote = lots !== null && ticks !== null ? quoteAtoms(lots, ticks, m) : null;
  el("tr-base-label").textContent = buy ? "You buy" : "You sell";
  el("tr-base").textContent = base === null ? "—" : amountText(base, m.base);
  el("tr-quote-label").textContent = buy ? "You pay at most" : "You receive at least";
  const fee = quote === null ? null : sellerFee(quote, info.fee_ppm);
  el("tr-quote").textContent = quote === null ? "—" : amountText(buy ? quote : quote - (fee ?? 0n), m.quote);
  el("tr-fee-row").hidden = buy;
  el("tr-fee-label").textContent = `Fee (${feeRate(info.fee_ppm)} of the sale)`;
  el("tr-fee").textContent = fee === null ? "—" : `${amountText(fee, m.quote)} at this price`;

  const need = buy ? quote : base;
  if (!error && need !== null && have !== null && need > have) {
    error = `This needs ${amountText(need, spend)}; ${amountText(have, spend)} is available.`;
  }
  const errorBox = el("tr-form-error");
  errorBox.hidden = error === "";
  errorBox.textContent = error;
  const blocked = gate();
  el("tr-gate").hidden = blocked === null;
  el("tr-gate").textContent = blocked ?? "";
  const button = el<HTMLButtonElement>("tr-review");
  button.textContent = `${buy ? "Buy" : "Sell"} ${m.base.native ? "B3" : m.base.name}`;
  button.disabled = blocked !== null || error !== "" || ticks === null || lots === null || lots === 0n;

  // The slider is a share of what the available balance pays for.
  const most = have !== null && (ticks !== null || !buy) ? maxLots(side, have, ticks ?? 0n, m) : 0n;
  const slider = el<HTMLInputElement>("tr-pct");
  slider.disabled = most === 0n;
  const pct = most > 0n && lots !== null ? Math.min(100, Number((lots * 100n) / most)) : 0;
  slider.value = String(pct);
  el("tr-slider").style.setProperty("--pct", `${pct}%`);
  el("tr-pct-out").textContent = `${pct}%`;
  for (const mark of document.querySelectorAll<HTMLElement>("#tr-slider [data-pct]")) mark.classList.toggle("on", Number(mark.dataset.pct) <= pct && pct > 0);

  el("tr-steps-help").textContent =
    tif === "gtc"
      ? "A limit order fills what it can at your price or better. The rest waits in the book until it fills or you cancel it."
      : tif === "ioc"
        ? "An immediate order fills what it can at your price or better at once. The rest is dropped."
        : "A post-only order only ever waits in the book. It is rejected if it could fill at once.";
}

function setShare(pct: number): void {
  const m = market;
  if (!m) return;
  const spend = side === "buy" ? m.quote : m.base;
  const have = available(spend);
  const ticks = parsePrice(el<HTMLInputElement>("tr-price").value.trim(), m);
  if (have === null || (side === "buy" && ticks === null)) return;
  const most = maxLots(side, have, ticks ?? 0n, m);
  el<HTMLInputElement>("tr-lots").value = ((most * BigInt(Math.round(pct))) / 100n).toString();
  renderTicket();
}

/** Steps the price by one tick, or the amount by one lot. */
function nudge(id: string, dir: number): void {
  const m = market;
  if (!m) return;
  const input = el<HTMLInputElement>(id);
  if (id === "tr-price") {
    const ticks = parsePrice(input.value.trim(), m) ?? (book && (side === "buy" ? book.asks[0] : book.bids[0]) ? BigInt((side === "buy" ? book.asks[0] : book.bids[0]).price_ticks) - BigInt(dir) : 0n);
    const next = ticks + BigInt(dir);
    if (next >= 1n) input.value = plain(price(next, m));
  } else {
    const lots = parseWhole(input.value.trim()) ?? 0n;
    const next = lots + BigInt(dir);
    if (next >= 1n) input.value = next.toString();
  }
  renderTicket();
}

function pickPrice(ticks: bigint): void {
  const m = market;
  if (!m) return;
  el<HTMLInputElement>("tr-price").value = plain(price(ticks, m));
  renderTicket();
  el("tr-lots").focus();
}

async function reviewOrder(): Promise<void> {
  const m = market;
  if (!m) return;
  const ticks = parsePrice(el<HTMLInputElement>("tr-price").value.trim(), m);
  const lots = parseWhole(el<HTMLInputElement>("tr-lots").value.trim());
  if (ticks === null || lots === null) return;
  const button = el<HTMLButtonElement>("tr-review");
  button.disabled = true;
  try {
    const id = await invoke<string>("fm2_place_draft", {
      order: { market: m.id, side, tif, price_ticks: ticks.toString(), lots: lots.toString() },
    });
    drafts.set(id, "order");
    banner("tr-result", "info", "Check the order in the approval window. Nothing is signed until you approve it there.");
  } catch (e) {
    banner("tr-result", "danger", String(e));
  } finally {
    renderTicket();
  }
}

async function cancelOrder(order: string, button: HTMLButtonElement): Promise<void> {
  button.disabled = true;
  try {
    drafts.set(await invoke<string>("fm2_cancel_draft", { order }), "cancel");
  } catch (e) {
    banner("tr-error", "danger", String(e));
    button.disabled = false;
  }
}

async function cancelAll(): Promise<void> {
  try {
    drafts.set(await invoke<string>("fm2_cancel_all_draft"), "cancel");
  } catch (e) {
    banner("tr-error", "danger", String(e));
  }
}

async function postAgain(label: string, button: HTMLButtonElement, result: HTMLElement): Promise<void> {
  button.disabled = true;
  try {
    const admissions = await invoke<Admission[]>("fm2_resend", { label });
    result.textContent = admissionText(admissions);
    void invoke("fm2_refresh").catch(() => {});
  } catch (e) {
    result.textContent = String(e);
  } finally {
    button.disabled = false;
  }
}

function admissionText(admissions: Admission[]): string {
  const took = admissions.filter((a) => a.accepted);
  if (took.length === admissions.length) return `Every seat has it (${took.length} of ${admissions.length}).`;
  const refused = admissions.filter((a) => !a.accepted);
  const why = [...new Set(refused.map((a) => (a.code ? `${a.code}: ${a.reason}` : a.reason)))].join("; ");
  return `${took.length} of ${admissions.length} seats have it. The others: ${why}`;
}

// ---- transfers -------------------------------------------------------------

function transferAsset(): Asset | null {
  const id = el<HTMLSelectElement>("tr-transfer-asset").value;
  return info?.assets.find((a) => a.id === id) ?? null;
}

function renderTransfer(): void {
  const asset = transferAsset();
  if (!asset) return;
  el("tr-transfer-amount-label").textContent = `Amount (${unit(asset)})`;
  const have = available(asset);
  el("tr-transfer-avbl").textContent = have === null ? "" : `Available: ${masked(amountText(have, asset))}`;
  const to = el<HTMLInputElement>("tr-transfer-to").value.trim();
  const text = el<HTMLInputElement>("tr-transfer-amount").value.trim();
  const atoms = text === "" ? null : parseAmount(text, asset);
  let error = "";
  if (to !== "" && !/^[0-9a-fA-F]{64}$/.test(to)) error = "An account is 64 hexadecimal characters.";
  else if (to !== "" && to.toLowerCase() === info?.account) error = "That is this account itself.";
  else if (text !== "" && (atoms === null || atoms === 0n)) error = asset.native ? "Enter an amount in B3, with at most 9 decimals." : "Enter a whole number of atoms.";
  else if (atoms !== null && have !== null && atoms > have) error = `${amountText(have, asset)} is available.`;
  const box = el("tr-transfer-error");
  box.hidden = error === "";
  box.textContent = error;
  el<HTMLButtonElement>("tr-transfer-review").disabled = gate() !== null || error !== "" || to === "" || atoms === null || atoms === 0n;
}

async function reviewTransfer(): Promise<void> {
  const asset = transferAsset();
  if (!asset) return;
  const atoms = parseAmount(el<HTMLInputElement>("tr-transfer-amount").value.trim(), asset);
  if (atoms === null) return;
  try {
    const id = await invoke<string>("fm2_transfer_draft", {
      to: el<HTMLInputElement>("tr-transfer-to").value.trim().toLowerCase(),
      asset: asset.id,
      amount: atoms.toString(),
    });
    drafts.set(id, "transfer");
    banner("tr-transfer-result", "info", "Check the transfer in the approval window. Nothing is signed until you approve it there.");
  } catch (e) {
    banner("tr-transfer-result", "danger", String(e));
  }
}

// ---- results ---------------------------------------------------------------

function report(what: "order" | "cancel" | "transfer", tone: string, text: string): void {
  if (what === "transfer") banner("tr-transfer-result", tone, text);
  else if (what === "order") banner("tr-result", tone, text);
  else banner("tr-error", tone === "danger" ? "danger" : "", tone === "danger" ? text : "");
}

/** The approval window decided a draft. True when it was this page's. */
export function tradeDecided(r: Decided): boolean {
  const what = drafts.get(r.id);
  if (!what) return false;
  drafts.delete(r.id);
  if (r.outcome === "rejected") {
    report(what, "", "");
  } else if (r.outcome === "fm2" && r.sent) {
    const sent = r.sent;
    if (sent.accepted > 0) {
      awaited.set(sent.label, what);
      report(what, "info", `Signed and posted: ${sent.accepted} of ${sent.admissions.length} seats took it. Waiting for a certified slot.`);
    } else {
      report(what, "warning", `Signed and stored, but no seat took it. ${admissionText(sent.admissions)} It is in Activity, where you can post it again.`);
    }
    if (what === "order") for (const id of ["tr-lots"]) el<HTMLInputElement>(id).value = "";
    if (what === "transfer") el<HTMLInputElement>("tr-transfer-amount").value = "";
  } else {
    report(what, "danger", `Nothing was sent. ${r.error ?? ""}`);
  }
  void refreshAccount();
  return true;
}

/** Tells the user the certified end of what they just signed. */
function reportEnds(): void {
  if (!account) return;
  for (const [label, what] of awaited) {
    const row = account.activity.find((r) => r.label === label);
    if (!row || row.state === "open") continue;
    awaited.delete(label);
    if (row.state === "expired") {
      report(what, "warning", `It expired at slot ${whole(BigInt(row.height))} without being included. Nothing happened.`);
    } else {
      report(what, row.accepted ? "success" : "danger", `Certified in slot ${whole(BigInt(row.height))}: ${resultText(row)}.`);
    }
  }
}

// ---- wiring ----------------------------------------------------------------

type BookEvent = { seat: number; market: string; slot: string; bids: Level[]; asks: Level[] };
type TradesEvent = { seat: number; market: string; slot: string; fills: { taker_side: "buy" | "sell"; price_ticks: string; lots: string; quote_atoms: string; mine: "maker" | "taker" | null }[] };

async function wire(): Promise<void> {
  await listen<{ channel: string; seat: number; live: boolean; detail: string }>("fm2-link", (event) => {
    links.set(event.payload.channel, event.payload);
    if (!active || !info) return;
    renderHead();
    if (event.payload.channel === "book") renderBook();
    if (event.payload.channel === "trades") renderTrades();
  });
  await listen<{ seat: number; height: string }>("fm2-slot", (event) => {
    slot = event.payload;
    if (active && info) el("tr-slot").textContent = whole(BigInt(slot.height));
  });
  await listen<BookEvent>("fm2-book", (event) => {
    if (event.payload.market !== market?.id) return;
    book = event.payload;
    if (!active) return;
    renderBook();
    renderHead();
    if (chartView === "depth") renderChart();
  });
  await listen<TradesEvent>("fm2-trades", (event) => {
    const p = event.payload;
    if (p.market !== market?.id || p.fills.length === 0) return;
    const now = Date.now();
    const fresh = p.fills.map((f) => ({ at: now, slot: p.slot, seat: p.seat, takerSide: f.taker_side, ticks: BigInt(f.price_ticks), lots: BigInt(f.lots), quote: BigInt(f.quote_atoms), mine: f.mine }));
    // Newest first, the last fill of the slot on top.
    trades = [...fresh.reverse(), ...trades].slice(0, TRADES_KEPT);
    if (fresh.some((t) => t.mine)) void refreshAccount();
    if (!active) return;
    renderTrades();
    renderHead();
    if (chartView === "price") renderChart();
    if (book) el("tr-mid-last").textContent = market ? price(trades[0].ticks, market) : "—";
  });
  await listen("fm2-account", () => void refreshAccount());
  await listen("fm2-activity", () => void refreshAccount());

  el("tr-connect-form").addEventListener("submit", (e) => {
    e.preventDefault();
    void connect();
  });
  for (const [button, input, what] of [
    ["tr-genesis-pick", "tr-genesis", "genesis"],
    ["tr-pins-pick", "tr-pins", "pins"],
  ] as const) {
    el(button).addEventListener("click", async () => {
      const picked = await invoke<string | null>("fm2_pick", { what }).catch(() => null);
      if (picked) el<HTMLInputElement>(input).value = picked;
    });
  }
  el("tr-disconnect").addEventListener("click", () => void disconnect());
  el("tr-key-create").addEventListener("click", async () => {
    const button = el<HTMLButtonElement>("tr-key-create");
    button.disabled = true;
    banner("tr-key-error", "danger", "");
    try {
      const created = await invoke<string>("fm2_key_create");
      if (info) info.account = created;
      // The account's own feed starts with the next watch.
      if (market) await invoke("fm2_watch", { market: market.id });
      await refreshAccount();
      renderAccount();
    } catch (e) {
      banner("tr-key-error", "danger", String(e));
    } finally {
      button.disabled = false;
    }
  });
  el("tr-account-copy").addEventListener("click", () => {
    if (!info?.account) return;
    const button = el("tr-account-copy");
    void navigator.clipboard.writeText(info.account).then(() => {
      button.textContent = "Copied";
      setTimeout(() => (button.textContent = "Copy"), 1500);
    });
  });
  el<HTMLSelectElement>("tr-market").addEventListener("change", (e) => void selectMarket((e.target as HTMLSelectElement).value));

  const TABS = [
    ["tr-tab-orders", "tr-orders"],
    ["tr-tab-trades", "tr-trades"],
    ["tr-tab-activity", "tr-activity"],
    ["tr-tab-transfer", "tr-transfer"],
  ] as const;
  for (const [tab] of TABS) {
    el(tab).addEventListener("click", () => {
      for (const [t, p] of TABS) {
        el(t).setAttribute("aria-selected", String(t === tab));
        el(p).hidden = t !== tab;
      }
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
  for (const t of ["gtc", "ioc", "post_only"] as const) {
    el(`tr-type-${t}`).addEventListener("click", () => {
      tif = t;
      renderTicket();
    });
  }
  for (const [id, s] of [
    ["tr-buy", "buy"],
    ["tr-sell", "sell"],
  ] as const) {
    el(id).addEventListener("click", () => {
      side = s;
      renderTicket();
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
  el("tr-form").addEventListener("input", (e) => {
    if ((e.target as HTMLElement).id !== "tr-pct") renderTicket();
  });
  el("tr-form").addEventListener("submit", (e) => {
    e.preventDefault();
    void reviewOrder();
  });
  el("tr-cancel-all").addEventListener("click", () => void cancelAll());
  el("tr-activity-refresh").addEventListener("click", async () => {
    const button = el<HTMLButtonElement>("tr-activity-refresh");
    button.disabled = true;
    await invoke("fm2_refresh").catch((e) => banner("tr-error", "danger", String(e)));
    await refreshAccount();
    button.disabled = false;
  });
  el("tr-transfer-form").addEventListener("input", renderTransfer);
  el("tr-transfer-asset").addEventListener("change", renderTransfer);
  el("tr-transfer-form").addEventListener("submit", (e) => {
    e.preventDefault();
    void reviewTransfer();
  });

  // Seats are asked for their status now and then while the page is open.
  setInterval(() => {
    if (active && info && Date.now() - healthRead > HEALTH_MS) void refreshHealth();
  }, 1000);
}
