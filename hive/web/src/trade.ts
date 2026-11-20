// The Trade page: a FlowMesh V2 test network, in two views of the same
// account and market.
//
// Simple: pay one thing, receive another. The page works out an immediate
// order from the live book, with a price limit, so a trade can never cost
// more than the card says. Pro: the book, the chart and orders at a price
// of the user's own.
//
// What is shown comes in two kinds, and the page says which is which:
// - confirmed: this account's balances and the result of everything it
//   signed. The core checked their proofs against the genesis document
//   before sending them here.
// - the live view: the order book and the trades, from one validator's
//   feed, which carries no proof. Everything read from it is an estimate.
//
// This page never signs. An order, a cancellation or a transfer is drafted
// by the core, which checks it again, and is signed only when it is
// approved in the approval window.
import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";

import { bannerTone, el, icon, isMasked } from "../shared/ui";
import { trapDialog } from "./a11y";
import {
  amount,
  amountText,
  assetPlaces,
  averageText,
  baseAtoms,
  depth,
  feeRate,
  lotPlaces,
  lotText,
  lotsIn,
  maxLots,
  nearestPrice,
  outcomeText,
  parseAmount,
  parsePrice,
  percent,
  plain,
  plainPrice,
  plan,
  price,
  priceText,
  priceUnit,
  quoteAtoms,
  reach,
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
  type Plan,
  type Want,
} from "./fm2fmt";
import { OWN_ORDER, WAITING, advise, money, numberProblem, outlook, size, tell, type Advice, type Fill, type Offer, type Row, type Told } from "./fm2words";

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
type Files = { genesis: string; pins: string };

/** A draft's end, as the main window receives it ("draft-result"). */
export type TradeOutcome = { outcome: "fm2"; id: string; sent: Sent };
type Decided = { outcome: string; id: string; error?: string; sent?: Sent };

type Trade = { at: number; slot: string; takerSide: "buy" | "sell"; ticks: bigint; lots: bigint; quote: bigint; mine: "maker" | "taker" | null };

// ---- state -----------------------------------------------------------------

/** The newest trades kept for the tape and the chart. */
const TRADES_KEPT = 300;
/** Book levels shown per side when both sides show. */
const BOOK_ROWS = 10;
/** The height of one row of the book, as the stylesheet sets it. */
const BOOK_ROW_PX = 22;
/** A row that is new in the book flashes for this long. */
const FLASH_MS = 800;
/** Validators are asked for their status this often while the page is open. */
const HEALTH_MS = 5000;
/** After this long without a confirmed end, the page says it is slow. */
const SLOW_MS = 10_000;
/** A trade whose last price is this far from the best one is called large,
 *  in hundredths of a percent. */
const FAR_BPS = 200n;
/** A limit price this far through the best one gets a warning. */
const THROUGH_BPS = 500n;
const MASK = "••••";

let active = false;
let wired = false;
let connecting = false;
/** The user asked for another network: do not reconnect to the last one. */
let leftOnPurpose = false;
let info: Info | null = null;
let market: Market | null = null;
let book: { bids: Level[]; asks: Level[]; slot: string; seat: number } | null = null;
let trades: Trade[] = [];
let account: AccountView | null = null;
let health: Health[] = [];
let healthRead = 0;
let slot: { height: string; seat: number } | null = null;
/** Whether each feed is live, and on which validator. */
const links = new Map<string, { seat: number; live: boolean; detail: string }>();

let mode: "simple" | "pro" = "simple";
/** Buying or selling the market's base asset, in both views. */
let side: "buy" | "sell" = "buy";
/** Simple: the field the user typed in; the other one follows. */
let driver: "pay" | "get" = "pay";
/** Simple: said once after the user rejected a trade, until they go on. */
let afterReject = false;
/** What the simple card last said, for the review that follows its button. */
let swapSaid: Advice | null = null;
/** The simple card's first field takes the cursor once, when it appears. */
let wantFocus = false;
/** What an order with a price of its own does with what cannot trade at
 *  once: wait (good till cancelled), drop it (immediate or cancel), or
 *  only ever wait (post only). */
let stay: "gtc" | "ioc" | "post_only" = "gtc";
/** How many columns the page has room for (hive.css: 740px and 1180px). */
let columns: 1 | 2 | 3 = 2;
/** Pro: an order at the user's price, or at once against the book. */
let kind: "limit" | "market" = "limit";
/** Pro: the user typed the total, so the amount follows it. */
let totalLed = false;
let bookShow: "both" | "bids" | "asks" = "both";
let chartView: "price" | "depth" | "trades" = "price";
let historyShown = 6;
const shown = { asks: new Set<string>(), bids: new Set<string>() };

/** What kind of thing was started, and so where on the page its end shows:
 *  a trade (in the view that is open), something about the waiting orders,
 *  or a transfer. */
type Origin = "trade" | "orders" | "account";
/** One thing the user started: reviewed, then sent, then confirmed. */
type Flow = {
  origin: Origin;
  /** What is under way, e.g. "Buying 0.005 B3". */
  verb: string;
  /** What the button says that takes the request back before it is
   *  approved. "Cancel", except for a cancellation, where that would read
   *  as its opposite. */
  back: string;
  /** What the order form asked for when the request was made. */
  form: string;
  stage: "review" | "sent" | "done";
  draft: string;
  label: string | null;
  sent: Sent | null;
  /** The confirmed end. */
  row: Row | null;
  /** Why nothing was sent. */
  error: string;
  at: number;
  slow: boolean;
};
const flows = new Map<Origin, Flow>();
/** Drafts this page asked for and the approval window has not decided. */
const drafts = new Map<string, Origin>();

// ---- small helpers ---------------------------------------------------------

const masked = (text: string): string => (isMasked() ? MASK : text);

function h<K extends keyof HTMLElementTagNameMap>(tag: K, cls = "", text = ""): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  if (cls) node.className = cls;
  if (text) node.textContent = text;
  return node;
}

function action(text: string, cls: string, run: () => void): HTMLButtonElement {
  const node = h("button", cls, text);
  node.type = "button";
  node.addEventListener("click", run);
  return node;
}

/** A link inside a sentence that does something on this page. */
const link = (text: string, run: () => void): HTMLButtonElement => action(text, "link", run);

/** Rebuilds `box` only when what it shows changed. The live view redraws
 *  the page about once a second; a redraw between a press and its release
 *  would swallow the click, and take the keyboard focus with it. */
function keyed(box: HTMLElement, key: string, build: () => Node[]): void {
  if (box.dataset.key === key) return;
  box.dataset.key = key;
  box.replaceChildren(...build());
}

/** One line of a definition list: what it is, and its value. */
function pair(key: string, value: string, cls = ""): HTMLDivElement {
  const line = h("div", cls);
  line.append(h("dt", "", key), h("dd", "", value));
  return line;
}

function cell(text: string, cls = ""): HTMLTableCellElement {
  return h("td", cls, text);
}

function banner(id: string, tone: string, text: string, extra = ""): void {
  const box = el(id);
  box.hidden = text === "";
  box.textContent = text;
  if (text) bannerTone(box, tone, extra);
}

/** The hive's cell with an asset's initial; B3's own is the honey one. */
function fillHex(node: HTMLElement, asset: Asset): void {
  node.className = asset.native ? "tr-hex tr-hex-accent" : "tr-hex";
  node.textContent = unit(asset).slice(0, 1).toUpperCase();
}

function hex(asset: Asset): HTMLSpanElement {
  const node = h("span");
  node.setAttribute("aria-hidden", "true");
  fillHex(node, asset);
  return node;
}

function options(select: HTMLSelectElement, items: { id: string; text: string }[], chosen: string): void {
  const key = items.map((item) => `${item.id}=${item.text}`).join("|");
  if (select.dataset.key !== key) {
    select.dataset.key = key;
    select.replaceChildren(
      ...items.map((item) => {
        const option = h("option", "", item.text);
        option.value = item.id;
        return option;
      }),
    );
  }
  select.value = chosen;
}

function copyTo(text: string, button: HTMLElement): void {
  const label = button.textContent;
  void navigator.clipboard.writeText(text).then(() => {
    button.textContent = "Copied";
    setTimeout(() => (button.textContent = label), 1500);
  });
}

function clock(ms: number): string {
  const d = new Date(ms);
  const two = (n: number) => String(n).padStart(2, "0");
  return `${two(d.getHours())}:${two(d.getMinutes())}:${two(d.getSeconds())}`;
}

/** A balance with its unit, with the decimals the asset trades in, so that
 *  it keeps its length from one trade to the next. */
const held = (atoms: bigint, asset: Asset): string => `${amount(atoms, asset, assetPlaces(asset, info?.markets ?? []))} ${unit(asset)}`;

/** What this account can spend of an asset; null when no validator has
 *  proved the account. */
function available(asset: Asset): bigint | null {
  if (!account?.proved) return null;
  const held = account.balances.find((b) => b.asset.id === asset.id);
  return held ? BigInt(held.available) : 0n;
}

/** The account exists on the network and its balances are proved. */
const ready = (): boolean => !!info?.account && !!account?.proved;

/** Something waits in the approval window: one at a time. */
const pending = (): boolean => [...flows.values()].some((f) => f.stage === "review");
/** Why a button is off while a request waits in the approval window. */
const ONE_AT_A_TIME = "Finish the request in the small window first. One request waits at a time.";

/** Whether the request that waits there was made from the trading form. */
const ownWaits = (): boolean => flows.get("trade")?.stage === "review";

/** How the network is doing, in words for the top of the page. */
function network(): { state: "ok" | "wait" | "bad"; text: string; notice: string } {
  if (!info) return { state: "wait", text: "", notice: "" };
  const all = info.seats.length;
  if (!health.length) return { state: "wait", text: "Connecting to the test network", notice: "" };
  const up = health.filter((x) => x.reachable && !x.fenced).length;
  if (up === 0) {
    return {
      state: "bad",
      text: "The test network is not answering",
      notice: "No validator of this test network is answering. It may not be running right now. This page keeps trying, and nothing you did is lost.",
    };
  }
  if (up < info.quorum) {
    return {
      state: "bad",
      text: `Only ${up} of ${all} validators are answering`,
      notice: `Only ${up} of this network's ${all} validators are answering, and ${info.quorum} must agree before anything is confirmed. Trading waits until enough of them are back.`,
    };
  }
  if (!links.get("slots")?.live) return { state: "wait", text: "Connecting to the test network", notice: "" };
  return { state: "ok", text: "Connected to a test network", notice: "" };
}

// ---- entering and leaving --------------------------------------------------

export function startTrade(): void {
  active = true;
  placeBar();
  void enter();
}

/** The page's bar (the network, and the choice of view) sits in the page's
 *  header while the Trade page is open and the header shows: the header
 *  has the room, and the page needs the height. */
const HEADER_SHOWS = matchMedia("(min-width: 640px)");
function placeBar(): void {
  const head = document.querySelector<HTMLElement>(".page-head");
  if (active && head && HEADER_SHOWS.matches) head.append(el("tr-bar"));
  else el("tr-notice").before(el("tr-bar"));
}
HEADER_SHOWS.addEventListener("change", placeBar);

export function stopTrade(): void {
  if (!active) return;
  active = false;
  placeBar();
  closeDialog();
  el<HTMLDetailsElement>("tr-net").open = false;
  invoke("fm2_unwatch").catch(() => {});
}

/** Re-render after amounts were masked or unmasked. */
export function tradeMaskChanged(): void {
  if (active && info) render();
}

async function enter(): Promise<void> {
  wantFocus = true;
  if (!wired) {
    wired = true;
    await wire();
    mode = (await invoke<boolean>("ui_trade_pro").catch(() => false)) ? "pro" : "simple";
  }
  try {
    info = await invoke<Info | null>("fm2_info");
  } catch (e) {
    info = null;
    banner("tr-error", "danger", String(e));
  }
  if (!active) return;
  if (info) {
    await opened();
    return;
  }
  const last = await invoke<Files | null>("fm2_last").catch(() => null);
  if (last && !leftOnPurpose) await connect(last, true);
  else showConnect(last);
}

function showConnect(last: Files | null): void {
  for (const id of ["tr-bar", "tr-strip", "tr-cols", "tr-notice"]) el(id).hidden = true;
  el("tr-connect").hidden = false;
  el("tr-connect-pick").hidden = false;
  el("tr-connect-busy").hidden = true;
  if (last) {
    el<HTMLInputElement>("tr-genesis").value = last.genesis;
    el<HTMLInputElement>("tr-pins").value = last.pins;
  }
}

/** Opens the network of two files. `again` is the page doing it by itself,
 *  with the files of the last time. */
async function connect(files: Files, again: boolean): Promise<void> {
  if (connecting) return;
  connecting = true;
  showConnect(files);
  el("tr-connect-pick").hidden = true;
  el("tr-connect-busy").hidden = false;
  el("tr-connect-busy-text").textContent = again ? "Connecting to the test network you used last." : "Connecting.";
  banner("tr-connect-error", "danger", "");
  try {
    info = await invoke<Info>("fm2_connect", files);
    leftOnPurpose = false;
    if (active) await opened();
  } catch (e) {
    info = null;
    el("tr-connect-pick").hidden = false;
    banner("tr-connect-error", "danger", again ? `The test network you used last could not be opened. ${e}` : String(e));
  } finally {
    connecting = false;
    el("tr-connect-busy").hidden = true;
  }
}

async function disconnect(): Promise<void> {
  el<HTMLDetailsElement>("tr-net").open = false;
  await invoke("fm2_disconnect").catch(() => {});
  const last = await invoke<Files | null>("fm2_last").catch(() => null);
  leftOnPurpose = true;
  info = null;
  market = null;
  account = null;
  book = null;
  trades = [];
  health = [];
  healthRead = 0;
  slot = null;
  links.clear();
  drafts.clear();
  flows.clear();
  banner("tr-error", "danger", "");
  banner("tr-connect-error", "danger", "");
  showConnect(last);
}

/** A connection is there: show its markets and follow one. */
async function opened(): Promise<void> {
  if (!info) return;
  banner("tr-error", "danger", "");
  el("tr-connect").hidden = true;
  for (const id of ["tr-bar", "tr-strip", "tr-cols"]) el(id).hidden = false;
  options(
    el<HTMLSelectElement>("tr-market"),
    info.markets.map((m) => ({ id: m.id, text: `${unit(m.base)} / ${unit(m.quote)}` })),
    "",
  );
  if (info.recovered_bytes > 0) {
    banner("tr-error", "warning", "B3 Hive's record of what you signed ended in a half-written entry, from a crash while it was being saved. That entry was dropped. It had not been sent.");
  }
  setMode(mode, false);
  if (info.markets.length === 0) {
    banner("tr-notice", "warning", "This network's genesis document lists no market, so there is nothing to trade on it.");
    return;
  }
  const keep = info.markets.find((m) => m.id === market?.id) ?? info.markets[0];
  await selectMarket(keep.id);
}

async function selectMarket(id: string): Promise<void> {
  if (!info) return;
  const next = info.markets.find((m) => m.id === id) ?? null;
  if (next?.id !== market?.id) {
    book = null;
    trades = [];
    shown.asks.clear();
    shown.bids.clear();
    clearInputs();
  }
  market = next;
  links.clear();
  el<HTMLSelectElement>("tr-market").value = market?.id ?? "";
  syncPair();
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

function clearInputs(): void {
  for (const id of ["tr-pay", "tr-get", "tr-price", "tr-amount", "tr-total"]) el<HTMLInputElement>(id).value = "";
  driver = "pay";
  totalLed = false;
  afterReject = false;
}

// ---- the two views ---------------------------------------------------------

function setMode(next: "simple" | "pro", save: boolean): void {
  mode = next;
  const pro = next === "pro";
  el("tr-page").dataset.mode = next;
  el("tr-mode-simple").setAttribute("aria-pressed", String(!pro));
  el("tr-mode-pro").setAttribute("aria-pressed", String(pro));
  // What is left to do takes the place of the form it stands in for.
  placeLists();
  el("tr-lists").hidden = false;
  if (pro) el("tr-col-side").prepend(el("tr-start"));
  else el("tr-col-main").prepend(el("tr-start"));
  for (const id of ["tr-orders", "tr-activity"]) {
    if (pro) el(id).setAttribute("role", "tabpanel");
    else el(id).removeAttribute("role");
  }
  // A finished trade's result does not follow into the other view; it is
  // in the history.
  if (save && flows.get("trade")?.stage === "done") flows.delete("trade");
  if (save) invoke("set_ui_trade_pro", { pro }).catch(() => {});
  render();
  // The other view is another page under the same header: it starts at its
  // top, not where the first one was left.
  if (save) document.scrollingElement?.scrollTo({ top: 0 });
}

/** The orders and the history sit under the account in Simple. In Pro
 *  they sit under the chart when the page has three columns and under the
 *  order book, above the chart, when it has two: the book, the order form
 *  and the open orders belong on one screen, and a chart that starts when
 *  the page connects can wait below them. In one column they follow the
 *  order form, which follows the book. */
function placeLists(): void {
  const lists = el("tr-lists");
  if (mode === "pro" && columns === 1) {
    if (lists.previousElementSibling !== el("tr-ticket")) el("tr-ticket").after(lists);
    return;
  }
  const home = el(mode === "simple" ? "tr-col-side" : columns === 3 ? "tr-col-mid" : "tr-col-main");
  if (lists.parentElement !== home || lists !== home.lastElementChild) home.append(lists);
}

/** Opens Pro on an order at a price of the user's own. */
function openLimit(): void {
  kind = "limit";
  setMode("pro", true);
  el("tr-price").focus();
}

// ---- what is traded for what -----------------------------------------------

/** The assets that can be paid with: every asset some market trades. */
function tradable(): Asset[] {
  const all = info;
  if (!all) return [];
  return all.assets.filter((a) => all.markets.some((m) => m.base.id === a.id || m.quote.id === a.id));
}

/** The assets `asset` can be traded for directly. */
function counterparts(asset: Asset): Asset[] {
  if (!info) return [];
  return info.markets.flatMap((m) => (m.base.id === asset.id ? [m.quote] : m.quote.id === asset.id ? [m.base] : []));
}

/** The pickers of the simple view follow the market and the side. */
function syncPair(): void {
  const m = market;
  if (!m) return;
  const pay = side === "buy" ? m.quote : m.base;
  const get = side === "buy" ? m.base : m.quote;
  const named = (a: Asset) => ({ id: a.id, text: unit(a) });
  options(el<HTMLSelectElement>("tr-pay-asset"), tradable().map(named), pay.id);
  options(el<HTMLSelectElement>("tr-get-asset"), counterparts(pay).map(named), get.id);
  fillHex(el("tr-pay-hex"), pay);
  fillHex(el("tr-get-hex"), get);
}

/** Paying `pay` to receive `get`: the market that trades them, and whether
 *  that buys or sells its base. */
function setPair(pay: string, get: string): void {
  if (!info) return;
  for (const m of info.markets) {
    const buys = m.quote.id === pay && m.base.id === get;
    if (!buys && !(m.base.id === pay && m.quote.id === get)) continue;
    side = buys ? "buy" : "sell";
    clearInputs();
    if (m.id !== market?.id) void selectMarket(m.id);
    else {
      syncPair();
      render();
    }
    return;
  }
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
    const read = await invoke<AccountView>("fm2_account");
    if (!info) return;
    account = read;
    if (account.account && !info.account) info.account = account.account;
    settleFlows();
    if (active) render();
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
  if (!active || !info) return;
  renderBar();
  renderStart();
  renderSwap();
  renderTicket();
}

// ---- rendering -------------------------------------------------------------

function render(): void {
  if (!info) return;
  renderBar();
  renderStart();
  renderStrip();
  renderBook();
  renderChart();
  renderTrades();
  renderAccount();
  renderOrders();
  renderActivity();
  renderSwap();
  renderTicket();
  renderFlows();
  takeCursor();
}

function renderBar(): void {
  if (!info) return;
  const net = network();
  el("tr-net").dataset.state = net.state;
  el("tr-net-text").textContent = net.text;
  el("tr-net-dot").classList.toggle("dot-pulse", net.state === "wait");
  if (info.markets.length) banner("tr-notice", "warning", net.notice);

  const all = info.seats.length;
  const up = health.filter((x) => x.reachable && !x.fenced).length;
  el("tr-net-lead").textContent = health.length
    ? `${up} of this network's ${all} validators are answering. A result counts once ${info.quorum} of them have signed it.`
    : `This network has ${all} validators. A result counts once ${info.quorum} of them have signed it.`;
  el("tr-validators").replaceChildren(
    ...info.seats.map((s) => {
      const x = health.find((y) => y.seat === s.seat);
      const item = h("li");
      item.dataset.state = !x ? "" : !x.reachable ? "bad" : x.fenced ? "warn" : "ok";
      item.title = `Seat ${s.seat} of the genesis document, at ${s.address}${x?.error ? `\n${x.error}` : ""}`;
      const dot = h("span", "dot");
      dot.setAttribute("aria-hidden", "true");
      const note = !x
        ? "not asked yet"
        : !x.reachable
          ? "not answering"
          : x.fenced
            ? "has stopped taking part"
            : x.published_height === null
              ? "answering"
              : `at slot ${whole(BigInt(x.published_height))}`;
      item.append(dot, h("span", "", `Validator ${s.seat + 1}`), h("span", "tr-v-note", note));
      return item;
    }),
  );
  const line = (key: string, value: string, title = "") => {
    const node = h("div", "drow");
    const shownValue = h("span", "dval", value);
    if (title) shownValue.title = title;
    node.append(h("span", "dkey", key), shownValue);
    return node;
  };
  el("tr-net-rows").replaceChildren(
    line("Latest confirmed slot", slot ? whole(BigInt(slot.height)) : "None yet", "A slot is one numbered step of the network, about a second long. This is the newest one whose signatures B3 Hive checked."),
    line("Network", shortId(info.domain), `Domain ${info.domain}\nValidator set ${info.seat_set}`),
    line("Rules", info.profile),
    line("Connections", "Each is pinned to its validator's key"),
  );
}

// ---- what is left to do before trading -------------------------------------

function step(state: "done" | "active" | "pending", number: number, title: string, ...body: Node[]): HTMLLIElement {
  const item = h("li", `ci ${state}`);
  const mark = h("span", "ci-mark");
  mark.setAttribute("aria-hidden", "true");
  if (state === "done") mark.append(icon("check", "i-sm"));
  else mark.textContent = String(number);
  const text = h("div", "ci-body");
  text.append(h("strong", "", title), ...body);
  item.append(mark, text);
  return item;
}

function renderStart(): void {
  const box = el("tr-start");
  const show = !!info && !ready();
  box.hidden = !show;
  el("tr-swap").hidden = !ready();
  el("tr-ticket").hidden = !ready();
  if (!show || !info) {
    delete box.dataset.step;
    return;
  }
  const answered = !!account && account.seats.some((s) => s.height !== null);
  const at = !info.account ? "account" : !account ? "reading" : answered ? "funds" : "silent";
  // Building it again would take the focus off its button and close the
  // operator's help, so only when the step changes.
  if (box.dataset.step === at) return;
  box.dataset.step = at;

  // An account that cannot be read yet may well hold funds: say what the
  // page is waiting for, not what to do.
  el("tr-start-title").textContent = at === "reading" ? "Looking up your account" : at === "silent" ? "Waiting for the network" : "Before you can trade";
  if (at === "reading" || at === "silent") {
    const wait = h("li", "tr-start-wait");
    const spinner = h("span", "spinner");
    spinner.setAttribute("aria-hidden", "true");
    wait.append(spinner, h("span", "muted", at === "reading" ? "One moment." : "No validator is answering, so B3 Hive cannot show your account yet. It keeps trying, and nothing you did is lost."));
    el("tr-start-steps").replaceChildren(wait);
    return;
  }
  const steps: HTMLLIElement[] = [step("done", 1, "Connect to a test network")];
  if (!info.account) {
    const act = h("div", "ci-act");
    const errorBox = h("div", "banner banner-danger");
    errorBox.hidden = true;
    const create = action("Create my account", "btn btn-primary", () => void createAccount(create, errorBox));
    const buttons = h("div", "btn-row");
    buttons.append(create);
    act.append(buttons, errorBox);
    steps.push(
      step(
        "active",
        2,
        "Create your trading account",
        h("p", "muted", "One click. B3 Hive makes a key for this test network and keeps it on this computer. It signs what you do here, and only after you approve it."),
        h("p", "t-sm muted", "The account holds test funds only. They have no value, so its key is kept on this computer without a password."),
        act,
      ),
      step("pending", 3, "Get test funds"),
    );
  } else {
    const act = h("div", "ci-act");
    const field = h("div", "copyfield copyfield--wrap");
    const copy = action("Copy", "btn btn-primary btn-sm", () => copyTo(info?.account ?? "", copy));
    field.append(h("span", "val", info.account), copy);
    act.append(
      field,
      h("p", "t-sm muted", "The number is safe to share: all anyone can do with it is send funds to this account."),
      h("p", "t-sm muted", "There is nothing else to do here. This page shows the funds by itself as soon as the network confirms them."),
      operatorHelp(),
    );
    const listed = assetNames();
    steps.push(
      step("done", 2, "Create your trading account"),
      step(
        "active",
        3,
        "Get test funds",
        h("p", "muted", `On a test network, funds come from whoever runs it: the person who gave you the network folder. Send them this account number and say which funds you want.${listed ? ` This network has ${listed}.` : ""}`),
        act,
      ),
    );
  }
  el("tr-start-steps").replaceChildren(...steps);
}

/** How an operator credits an account, for the one who runs the network. */
function operatorHelp(): HTMLDetailsElement {
  const box = h("details", "disc disc--boxed");
  const summary = h("summary");
  summary.append(icon("chevron-right", "i-sm caret"), h("span", "", "I run this network myself"));
  const body = h("div", "disc-body stack t-sm");
  body.append(h("p", "muted", "Credit the account with the node's test-chain tool. Amounts are in the asset's smallest unit: one B3 is 1 000 000 000."), h("code", "tr-cmd t-xs", faucet()));
  box.append(summary, body);
  return box;
}

const faucet = (): string => `fm2-devchain --file <devchain.log> --genesis <genesis.json> deposit ${info?.account ?? "<account>"} <asset> <amount>`;

async function createAccount(button: HTMLButtonElement, errorBox: HTMLElement): Promise<void> {
  button.disabled = true;
  errorBox.hidden = true;
  try {
    const created = await invoke<string>("fm2_key_create");
    if (!info) return;
    info.account = created;
    // The account's own feed starts with the next watch.
    if (market) await invoke("fm2_watch", { market: market.id });
    await refreshAccount();
    render();
  } catch (e) {
    errorBox.textContent = String(e);
    errorBox.hidden = false;
    button.disabled = false;
  }
}

// ---- Simple: pay one thing, receive another --------------------------------

/** What the simple card asks for right now, and the order that does it. */
type Ask = {
  m: Market;
  buy: boolean;
  pay: Asset;
  get: Asset;
  /** The balance of the asset paid with. */
  have: bigint;
  /** Why the typed text is not an amount; empty when it is one, or empty. */
  problem: string;
  want: Want | null;
  p: Plan | null;
};

function ask(): Ask | null {
  const m = market;
  if (!m || !info) return null;
  const buy = side === "buy";
  const pay = buy ? m.quote : m.base;
  const get = buy ? m.base : m.quote;
  const have = available(pay) ?? 0n;
  const typedIn = driver === "pay" ? pay : get;
  const typed = el<HTMLInputElement>(driver === "pay" ? "tr-pay" : "tr-get").value.trim();
  let problem = "";
  let want: Want | null = null;
  if (typed !== "") {
    const atoms = parseAmount(typed, typedIn);
    if (atoms === null) problem = numberProblem(typed, typedIn);
    else if (atoms > 0n) want = { of: typedIn.id === m.base.id ? "base" : "quote", atoms };
  }
  const p = want && book ? plan(side, want, buy ? book.asks : book.bids, have, m, info.fee_ppm) : null;
  return { m, buy, pay, get, have, problem, want, p };
}

/** Whether an order of this account rests where an immediate order would
 *  trade: it would trade with itself. */
function meetsOwn(m: Market, buy: boolean, limit: bigint): boolean {
  return (account?.orders ?? []).some(
    (o) => o.market_id === m.id && o.side === (buy ? "sell" : "buy") && (buy ? BigInt(o.price_ticks) <= limit : BigInt(o.price_ticks) >= limit),
  );
}

/** Puts exactly `lots` on the simple card, in the field that states them. */
function useLots(lots: bigint): void {
  const m = market;
  if (!m) return;
  driver = side === "buy" ? "get" : "pay";
  el<HTMLInputElement>(side === "buy" ? "tr-get" : "tr-pay").value = plain(baseAtoms(lots, m), m.base);
  afterReject = false;
  renderSwap();
}

/** A piece of the sentence under the card: words, or a link that acts. */
type Part = string | { link: string; run: () => void };

/** What taking up an offer of the line under the card does. */
function taking(offer: Offer): () => void {
  if (offer.act === "use") {
    const lots = offer.lots;
    return () => useLots(lots);
  }
  return offer.act === "funds" ? openFunds : openLimit;
}

function renderSwap(): void {
  const q = ask();
  if (!q || !info || !ready()) return;
  const { m, buy, pay, get, have, p } = q;
  el("tr-swap").dataset.side = side;
  for (const [id, text, on] of [
    ["tr-swap-buy", `Buy ${unit(m.base)}`, buy],
    ["tr-swap-sell", `Sell ${unit(m.base)}`, !buy],
  ] as const) {
    el(id).textContent = text;
    el(id).setAttribute("aria-selected", String(on));
  }
  el("tr-pay-have").textContent = `You have ${masked(held(have, pay))}`;
  el("tr-get-have").textContent = `You have ${masked(held(available(get) ?? 0n, get))}`;
  const turn = `Turn it around: pay ${unit(get)}, receive ${unit(pay)}`;
  el("tr-flip-text").textContent = turn;
  el("tr-flip").title = turn;

  // The field the user did not type in follows the other.
  const filled = !!p && p.lots > 0n;
  const net = p ? p.gross - p.fee : 0n;
  const follows = el<HTMLInputElement>(driver === "pay" ? "tr-get" : "tr-pay");
  let derived = "";
  if (p && filled) {
    if (driver === "pay") derived = buy ? plain(baseAtoms(p.lots, m), m.base) : plain(net, m.quote);
    else derived = buy ? plain(p.gross, m.quote) : plain(baseAtoms(p.lots, m), m.base);
  }
  if (follows.value !== derived) follows.value = derived;
  const offers = book ? (buy ? book.asks : book.bids) : [];
  // The best price of the other direction: what selling pays while this
  // card buys, and the other way round.
  const across = book ? (buy ? book.bids : book.asks) : [];
  // What a budget that did not come out even could buy with one piece more.
  const up = p && p.lots > 0n && p.short === null && buy && q.want?.of === "quote" ? plan(side, { of: "base", atoms: baseAtoms(p.lots + 1n, m) }, offers, have, m, info.fee_ppm) : null;
  const said = advise({
    m,
    buy,
    pay,
    have,
    problem: q.problem,
    want: q.want,
    plan: p,
    more: up && up.short === null ? up : null,
    best: offers.length ? BigInt(offers[0].price_ticks) : 0n,
    first: offers.length ? BigInt(offers[0].lots) : 0n,
    other: across.length ? BigInt(across[0].price_ticks) : 0n,
    own: !!p && p.lots > 0n && meetsOwn(m, buy, p.limit),
    blocked: pending() ? (ownWaits() ? "review" : "elsewhere") : network().state === "bad" ? "network" : !book || !links.get("book")?.live ? "prices" : "",
    feePpm: info.fee_ppm,
  });
  swapSaid = said;
  // A typed budget is a ceiling when it does not come out even, and what a
  // trade costs or brings is an estimate when it reaches past the best
  // price. The labels say so only then.
  const sure = !p || p.limit === p.best;
  const spare = buy && driver === "pay" && said.enabled && !!q.want && !!p && q.want.atoms > p.ceiling;
  el("tr-pay-label").textContent = !buy ? "You sell" : spare ? "You pay up to" : said.enabled && !sure && driver === "get" ? "You pay about" : "You pay";
  el("tr-get-label").textContent = !buy && said.enabled && !sure ? "You receive about" : "You receive";

  el("tr-quote").replaceChildren(
    ...said.lines.map((line) => {
      const row = pair(line.key, line.value);
      if (line.note) row.append(h("span", "tr-quote-note", line.note));
      return row;
    }),
  );
  el("tr-swap-promise").textContent = said.promise;

  const hint: Part[] = said.hint.map((part) => (typeof part === "string" ? part : { link: part.link, run: taking(part) }));
  keyed(el("tr-swap-hint"), hint.map((part) => (typeof part === "string" ? part : `[${part.link}]`)).join(""), () =>
    hint.map((part) => (typeof part === "string" ? document.createTextNode(part) : link(part.link, part.run))),
  );
  // What became of a request that was just taken back: in a box of its
  // own above the button, where the eye is when the form returns.
  banner("tr-swap-note", "info", afterReject ? "You did not approve it, so nothing happened and no money moved." : "", "tr-inline");
  // The warning, with the amount that avoids it at its end.
  const warnBox = el("tr-swap-warn");
  warnBox.hidden = said.warn.length === 0;
  if (said.warn.length) bannerTone(warnBox, "warning", "tr-inline");
  keyed(warnBox, said.warn.map((part) => (typeof part === "string" ? part : `[${part.link}]`)).join(""), () => {
    // One run of text, so that the link stays in the sentence it ends.
    const text = h("span");
    text.append(...said.warn.map((part) => (typeof part === "string" ? part : link(part.link, taking(part)))));
    return [text];
  });

  // The button names the trade and its money, and says that pressing it is
  // not yet the trade.
  el("tr-swap-go-text").textContent = said.go;
  el("tr-swap-go-sub").hidden = !said.enabled;
  el<HTMLButtonElement>("tr-swap-go").disabled = !said.enabled;
  const idle = have === 0n || !offers.length;
  for (const chip of document.querySelectorAll<HTMLButtonElement>("#tr-swap .tr-chip")) chip.disabled = idle;
}

/** The first thing to do on the simple card is to type an amount: the
 *  cursor waits there when the card appears and when a request ends,
 *  unless the user is somewhere else already. */
function takeCursor(): void {
  if (!wantFocus) return;
  const field = el<HTMLInputElement>("tr-pay");
  // The card is not there yet (the first steps, a request under way).
  if (mode === "simple" && (flows.has("trade") || field.closest("[hidden]"))) return;
  wantFocus = false;
  // A dialog of this page is open: the cursor belongs to it.
  if (mode !== "simple" || dialog) return;
  const at = document.activeElement;
  const busy = at instanceof HTMLElement && at !== document.body && at.isConnected && !at.closest("[hidden]") && (el("view-trade").contains(at) || el("tr-bar").contains(at));
  if (!busy) field.focus({ preventScroll: true });
}

/** A share of what the account holds of the asset it pays with. */
function share(pct: bigint): void {
  const m = market;
  if (!m) return;
  const pay = side === "buy" ? m.quote : m.base;
  let atoms = ((available(pay) ?? 0n) * pct) / 100n;
  // The base trades in whole steps; what is left of one would not sell.
  if (side === "sell") atoms = baseAtoms(lotsIn(atoms, m), m);
  driver = "pay";
  afterReject = false;
  el<HTMLInputElement>("tr-pay").value = atoms > 0n ? plain(atoms, pay) : "";
  renderSwap();
}

async function reviewSwap(): Promise<void> {
  const q = ask();
  if (!q?.p || q.p.lots === 0n || q.p.short !== null) return;
  const { m, buy, p } = q;
  if (!swapSaid?.enabled) return;
  await offer(
    "trade",
    buy ? `Buying${swapSaid.go.slice(3)}` : `Selling${swapSaid.go.slice(4)}`,
    () => invoke<string>("fm2_place_draft", { order: { market: m.id, side, tif: "ioc", price_ticks: p.limit.toString(), lots: p.lots.toString() } }),
    buy ? "Don't buy" : "Don't sell",
  );
}

// ---- from review to the confirmed end --------------------------------------

/** Asks the core for a draft and sends the user to the approval window. */
async function offer(origin: Origin, verb: string, draft: () => Promise<string>, back = "Cancel"): Promise<void> {
  if (pending()) return;
  const flow: Flow = { origin, verb, back, form: ticketState(), stage: "review", draft: "", label: null, sent: null, row: null, error: "", at: Date.now(), slow: false };
  // Set before the core answers, so a second click finds it.
  flows.set(origin, flow);
  afterReject = false;
  render();
  try {
    flow.draft = await draft();
    drafts.set(flow.draft, origin);
  } catch (e) {
    flow.stage = "done";
    flow.error = String(e);
  }
  render();
}

/** The approval window decided a draft. True when it was this page's. */
export function tradeDecided(r: Decided): boolean {
  const origin = drafts.get(r.id);
  if (!origin) return false;
  drafts.delete(r.id);
  const flow = flows.get(origin);
  if (!flow || flow.draft !== r.id) return true;
  if (r.outcome === "rejected") {
    flows.delete(origin);
    afterReject = origin === "trade";
    wantFocus = afterReject;
  } else if (r.outcome === "fm2" && r.sent) {
    flow.stage = "sent";
    flow.sent = r.sent;
    flow.label = r.sent.label;
    flow.at = Date.now();
  } else {
    flow.stage = "done";
    flow.error = r.error ?? "It could not be signed.";
  }
  if (active && info) render();
  void refreshAccount();
  return true;
}

/** A flow ends when the account's record shows its confirmed end. */
function settleFlows(): void {
  if (!account) return;
  for (const flow of flows.values()) {
    if (flow.stage !== "sent" || !flow.label) continue;
    const row = account.activity.find((r) => r.label === flow.label);
    if (row && row.state !== "open") {
      flow.stage = "done";
      flow.row = row;
    }
  }
}

/** How an order that went into the book stands now, as far as B3 Hive
 *  has seen: still waiting, or gone from the book. Nothing when its list of
 *  open orders cannot be trusted. */
function standing(label: string, m: Market): string {
  if (!account?.proved || account.orders_mismatch) return "";
  const o = account.orders.find((x) => x.label === label);
  if (!o) return "It is no longer waiting: it has traded or was cancelled.";
  const rested = BigInt(o.rested_lots);
  const left = BigInt(o.remaining_lots);
  const since = rested > left ? rested - left : 0n;
  // A waiting order trades at its own price.
  return since > 0n ? `Since then ${size(since, m)} of it has traded at ${priceText(BigInt(o.price_ticks), m)}, by the live view, and ${size(left, m)} is still waiting.` : "It is still waiting.";
}

function resultBox(tone: Told["result"], title: string, detail: string): HTMLDivElement {
  const box = h("div", "tr-flow-result");
  box.dataset.tone = tone;
  const text = h("div");
  text.append(h("strong", "", title));
  if (detail) text.append(h("span", "", detail));
  box.append(icon(tone === "success" ? "check-circle" : "alert-circle"), text);
  return box;
}

/** The mark over a trade's steps in the simple view: the hive's cell, in
 *  the colour of how things stand. */
function flowMark(tone: "window" | "wait" | Told["result"]): HTMLElement {
  const node = h("span", "tr-hex tr-flow-mark");
  node.dataset.tone = tone;
  node.setAttribute("aria-hidden", "true");
  node.append(tone === "wait" ? h("span", "spinner") : icon(tone === "window" ? "external" : tone === "success" ? "check" : "alert"));
  return node;
}

function showApproval(): void {
  invoke("approval_show").catch(() => {});
}

/** Takes back a request that waits for approval. The core rejects it as
 *  the approval window would, and this page hears of it the same way. */
async function withdraw(flow: Flow, button: HTMLButtonElement): Promise<void> {
  if (!flow.draft) return;
  button.disabled = true;
  try {
    await invoke("fm2_withdraw", { id: flow.draft });
  } catch {
    // Decided in the window a moment ago: its result is on its way here.
  }
}

async function sendAgain(flow: Flow, button: HTMLButtonElement): Promise<void> {
  if (!flow.label) return;
  button.disabled = true;
  try {
    const admissions = await invoke<Admission[]>("fm2_resend", { label: flow.label });
    if (flow.sent) {
      flow.sent.admissions = admissions;
      flow.sent.accepted = admissions.filter((a) => a.accepted).length;
    }
    void invoke("fm2_refresh").catch(() => {});
  } catch {
    // The core's own pass asks again; the row stays in the history.
  }
  button.disabled = false;
  renderFlows();
}

/** What the order form asks for, so that a form that was changed since a
 *  request was made from it can be told from one that was not. */
function ticketState(t: Ticket | null = ticket()): string {
  if (!t) return "";
  const limit = kind === "limit";
  return [t.m.id, side, kind, limit ? stay : "", limit ? t.ticks : "", t.lots].join("|");
}

const flowBox = (origin: Origin): string => (origin === "trade" ? (mode === "simple" ? "tr-swap-flow" : "tr-ticket-flow") : origin === "orders" ? "tr-orders-flow" : "tr-account-flow");

function renderFlows(): void {
  // A trade's flow shows in the view that is open.
  const other = el(mode === "simple" ? "tr-ticket-flow" : "tr-swap-flow");
  other.hidden = true;
  delete other.dataset.key;
  other.replaceChildren();
  const trading = flows.has("trade");
  // The card keeps its size while a trade's steps stand in for the form.
  const form = el("tr-swap-form");
  if (trading && mode === "simple" && !form.hidden) el("tr-swap-flow").style.minHeight = `${form.offsetHeight}px`;
  form.hidden = trading;
  el("tr-how").hidden = trading;
  for (const origin of ["trade", "orders", "account"] as const) renderFlow(origin);
}

function renderFlow(origin: Origin): void {
  const box = el(flowBox(origin));
  const flow = flows.get(origin);
  const full = origin === "trade" && mode === "simple";
  box.hidden = !flow;
  if (!flow) {
    delete box.dataset.key;
    box.replaceChildren();
    return;
  }
  // A result is told as things stand now: an order it says is waiting
  // may have traded or been cancelled since, and then it says so.
  const told = flow.stage === "done" && flow.row ? tell(flow.row, standing).detail : "";
  const key = [flow.stage, flow.draft, flow.sent?.accepted ?? "", flow.slow, flow.row?.state ?? "", flow.error, full, told].join("|");
  keyed(box, key, () => buildFlow(flow, full));
}

function buildFlow(flow: Flow, full: boolean): Node[] {
  const done = () => {
    flows.delete(flow.origin);
    wantFocus = flow.origin === "trade";
    render();
  };
  const nodes: Node[] = [];
  if (flow.stage === "review") {
    if (full) {
      const what = flow.verb.charAt(0).toLowerCase() + flow.verb.slice(1);
      nodes.push(
        flowMark("window"),
        h("h3", "", "Check it in the small window"),
        h("p", "muted", `A small window opened so you can check everything before ${what}. Approve it there to go ahead. No money moves until you do.`),
      );
      const buttons = h("div", "btn-row");
      const cancel = action(flow.back, "btn btn-ghost", () => void withdraw(flow, cancel));
      cancel.disabled = !flow.draft;
      buttons.append(action("Show the window again", "btn btn-secondary", showApproval), cancel);
      nodes.push(buttons);
    } else {
      const line = h("div", "tr-flow-line");
      const cancel = link(flow.back, () => void withdraw(flow, cancel));
      cancel.disabled = !flow.draft;
      cancel.title = "Takes the request back. Nothing is sent.";
      const acts = h("span", "tr-flow-links");
      acts.append(link("Show the window", showApproval), cancel);
      // The order form shows the order itself: its line need not repeat it.
      const lead = flow.origin === "trade" ? "Check" : `${flow.verb}: check`;
      line.append(h("span", "tr-flow-text", `${lead} it in the approval window. Nothing happens until you approve it there.`), acts);
      nodes.push(line);
    }
    return nodes;
  }
  if (flow.stage === "sent" && flow.sent) {
    const all = flow.sent.admissions.length;
    const took = flow.sent.accepted;
    if (full) {
      nodes.push(flowMark("wait"), h("h3", "", flow.verb));
      const list = h("ol", "checklist");
      list.append(step("done", 1, "You approved it, and B3 Hive signed it"));
      if (took > 0) {
        const waiting = step("active", 3, "Waiting for the network to confirm it");
        const spinner = h("span", "spinner");
        waiting.querySelector(".ci-mark")?.replaceChildren(spinner);
        list.append(step("done", 2, `Sent to the network: ${took} of ${all} validators have it`), waiting);
      } else {
        list.append(step("active", 2, "No validator has taken it yet"));
      }
      nodes.push(list);
    } else {
      const line = h("div", "tr-flow-line");
      const spinner = h("span", "spinner");
      spinner.setAttribute("aria-hidden", "true");
      line.append(spinner, h("span", "tr-flow-text", took > 0 ? `${flow.verb}: sent, waiting for the network to confirm it.` : `${flow.verb}: signed, but no validator has taken it yet.`));
      nodes.push(line);
    }
    const buttons = h("div", "btn-row");
    if (took === 0) {
      const reasons = [...new Set(flow.sent.admissions.map((a) => a.reason).filter(Boolean))].join("; ");
      nodes.push(h("p", "muted", `It is signed and stored on this computer, so it is not lost. ${reasons ? `The validators said: ${reasons}. ` : ""}It stays valid for about a minute.`));
      const again = action("Send it again", "btn btn-secondary btn-sm", () => void sendAgain(flow, again));
      buttons.append(again);
    } else if (flow.slow) {
      nodes.push(h("p", "muted", "This is taking longer than usual. It stays valid for about a minute. There is nothing you need to do, and its result will be in your history either way."));
    }
    if (full && (took === 0 || flow.slow)) buttons.append(action("Back to the form", "btn btn-ghost btn-sm", done));
    if (buttons.childElementCount) nodes.push(buttons);
    return nodes;
  }
  const told = flow.row ? tell(flow.row, standing) : null;
  const title = told ? told.title : "Nothing was sent";
  const said = told ? [told.lead, told.detail].filter(Boolean).join(" ") : flow.error;
  const buttons = h("div", "btn-row");
  if (full) {
    nodes.push(flowMark(told ? told.result : "danger"), h("h3", "", title));
    if (said) nodes.push(h("p", "muted", said));
    buttons.append(
      action(told?.result === "success" ? "Close" : "Back to the form", "btn btn-secondary", () => {
        // After a trade that went through, the card starts empty again.
        if (told?.result === "success") clearInputs();
        done();
      }),
    );
  } else {
    // Closed from its own corner: a row for a button would push down
    // what stands under it.
    const box = resultBox(told ? told.result : "danger", title, said);
    const close = action("", "tr-flow-close", done);
    close.setAttribute("aria-label", "Dismiss");
    close.title = "Dismiss";
    close.append(icon("close", "i-sm"));
    box.append(close);
    nodes.push(box);
  }
  if (buttons.childElementCount) nodes.push(buttons);
  return nodes;
}

// ---- Pro: the market -------------------------------------------------------

function renderStrip(): void {
  const m = market;
  if (!m || !info) return;
  const last = trades[0];
  el("tr-last").textContent = last ? price(last.ticks, m) : "—";
  // Coloured as the list of trades colours it: by who took whose price.
  el("tr-last").className = `tr-last tnum ${last ? (last.takerSide === "buy" ? "tr-up" : "tr-down") : ""}`;
  el("tr-last-unit").textContent = last ? `Last price, ${priceUnit(m)}` : "No trade seen yet";
  const pick = (id: string, level: Level | undefined) => {
    const button = el<HTMLButtonElement>(id);
    button.textContent = level ? price(BigInt(level.price_ticks), m) : "—";
    button.disabled = !level;
  };
  pick("tr-best-bid", book?.bids[0]);
  pick("tr-best-ask", book?.asks[0]);
  el("tr-lot").textContent = lotText(m);
  el("tr-tick").textContent = tickText(m);
  // A sale that brings anything pays at least the smallest unit.
  el("tr-fee-stat").textContent = `${feeRate(info.fee_ppm)}, at least ${amountText(1n, m.quote)} per order`;
}

/** How many prices of a side the book shows: ten where it has a column
 *  of its own. Beside the order form it is as tall as the form in the
 *  smallest window, so that the open orders under it start on the first
 *  screen: five prices there, and one more for each two rows a taller
 *  window adds. Above the form, in one column, five: the form comes next. */
function bookRows(): number {
  if (columns === 3) return BOOK_ROWS;
  if (columns === 1) return BOOK_ROWS / 2;
  const spare = Math.max(0, document.documentElement.clientHeight - 720);
  return Math.min(BOOK_ROWS, BOOK_ROWS / 2 + Math.floor(spare / (2 * BOOK_ROW_PX)));
}
/** The count the book was last drawn with. */
let bookDepth = 0;

function renderBook(): void {
  const m = market;
  const asks = el("tr-asks");
  const bids = el("tr-bids");
  el("tr-book-stale").hidden = !book || !!links.get("book")?.live;
  el("tr-book-price").textContent = m ? `Price (${unit(m.quote)})` : "Price";
  el("tr-book-amount").textContent = m ? `Amount (${unit(m.base)})` : "Amount";
  el("tr-book-sum").textContent = m ? `Sum (${unit(m.base)})` : "Sum";
  el("tr-book-note").textContent = book ? "Sellers above, buyers below. Click a price to use it. A dot marks your own order." : "Waiting for the live view.";
  const live = el("tr-book-live");
  live.title = `What one validator reports right now${book ? `, as of slot ${book.slot}` : ""}. It carries no proof, so every number in it is an estimate, not yet confirmed by the network.`;
  if (!m || !book) {
    asks.replaceChildren();
    bids.replaceChildren();
    el("tr-mid-last").textContent = "—";
    el("tr-spread").textContent = "";
    return;
  }
  const perSide = bookRows();
  bookDepth = perSide;
  // Twice as many for one side alone.
  const rows = bookShow === "both" ? perSide : 2 * perSide;
  const askLevels = depth(book.asks).slice(0, rows);
  const bidLevels = depth(book.bids).slice(0, rows);
  if (book.asks.length > rows || book.bids.length > rows) el("tr-book-note").textContent += ` The best ${rows} prices of a side are shown.`;
  // What this account has waiting at each price.
  const own = new Map<string, bigint>();
  for (const o of account?.orders ?? []) {
    if (o.market_id !== m.id) continue;
    const at = `${o.side === "sell" ? "asks" : "bids"}:${o.price_ticks}`;
    own.set(at, (own.get(at) ?? 0n) + BigInt(o.remaining_lots));
  }
  const places = lotPlaces(m);
  // One depth scale for both sides.
  const deepest = [...askLevels, ...bidLevels].reduce((max, l) => (l.total > max ? l.total : max), 0n);
  const row = (level: (typeof askLevels)[number], which: "asks" | "bids", previous: Set<string>) => {
    const key = level.ticks.toString();
    // A button, so the keyboard can take a level's price too.
    const button = h("button", `tr-book-row tr-pick tr-${which === "asks" ? "ask" : "bid"}`);
    button.type = "button";
    button.dataset.level = `${which}:${key}`;
    const mine = own.get(`${which}:${key}`);
    if (mine !== undefined) button.classList.add("tr-own");
    if (previous.size && !previous.has(key)) {
      button.classList.add("tr-flash");
      setTimeout(() => button.classList.remove("tr-flash"), FLASH_MS);
    }
    const part = deepest > 0n ? Number((level.total * 1000n) / deepest) / 10 : 0;
    button.style.setProperty("--depth", `${part}%`);
    button.append(
      h("span", "tr-price", price(level.ticks, m)),
      h("span", "num", amount(baseAtoms(level.lots, m), m.base, places)),
      h("span", "num", amount(baseAtoms(level.total, m), m.base, places)),
    );
    const yours = mine !== undefined ? `, ${size(mine, m)} of it yours` : "";
    button.title = `${which === "asks" ? "Sellers" : "Buyers"}: ${level.orders} order${level.orders === 1 ? "" : "s"} at this price${yours}. ${size(level.total, m)} from the best price to here.`;
    button.setAttribute("aria-label", `${which === "asks" ? "Sellers at" : "Buyers at"} ${priceText(level.ticks, m)}, ${size(level.lots, m)}${yours}. Use this price.`);
    button.addEventListener("click", () => pickPrice(level.ticks));
    return button;
  };
  // Redrawing must not take the keyboard focus off the level it was on.
  const focused = (document.activeElement as HTMLElement | null)?.dataset.level;
  // Sellers above the spread, the lowest price nearest it.
  asks.replaceChildren(...[...askLevels].reverse().map((l) => row(l, "asks", shown.asks)));
  bids.replaceChildren(...bidLevels.map((l) => row(l, "bids", shown.bids)));
  if (focused) el("tr-book").querySelector<HTMLElement>(`[data-level="${focused}"]`)?.focus();
  shown.asks = new Set(askLevels.map((l) => l.ticks.toString()));
  shown.bids = new Set(bidLevels.map((l) => l.ticks.toString()));

  el("tr-mid-last").textContent = trades.length ? price(trades[0].ticks, m) : "—";
  const gap = spread(book.bids, book.asks);
  el("tr-spread").textContent = gap
    ? `Spread ${price(gap.ticks, m)} (${percent(gap.bps)})`
    : book.bids.length && book.asks.length
      ? "The best prices meet"
      : book.bids.length
        ? "No sellers"
        : book.asks.length
          ? "No buyers"
          : "No orders are waiting";
}

const SVG = "http://www.w3.org/2000/svg";
const [CHART_W, CHART_H, CHART_PAD] = [600, 260, 14];
/** Prices take the top of the chart; the amounts traded the bottom. */
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
  const showTape = chartView === "trades";
  // The list of trades takes the chart's place in the card.
  el("tr-chart-box").hidden = showTape;
  el("tr-chart-foot").hidden = showTape;
  el("tr-trades").hidden = !showTape;
  // SVG elements have no hidden property: set the attribute.
  el("tr-chart").toggleAttribute("hidden", !showPrice);
  el("tr-depth").toggleAttribute("hidden", chartView !== "depth");
  for (const view of ["price", "depth", "trades"]) el(`tr-view-${view}`).setAttribute("aria-selected", String(view === chartView));
  const caption = el("tr-chart-caption");
  caption.textContent = showTape ? "Since this page connected, newest first" : showPrice ? "Trades since this page connected" : "Orders waiting, by price";
  caption.title = showTape
    ? "Each row is one trade the live view reported. FlowMesh keeps no list of earlier trades."
    : showPrice
      ? "Each point is one trade the live view reported, placed by the time it arrived. FlowMesh keeps no price history, so the chart starts when this page connects."
      : "The running total waiting at each price, as the live view reports the book.";
  for (const id of ["tr-chart-high", "tr-chart-low", "tr-chart-first", "tr-chart-mid", "tr-chart-last"]) el(id).textContent = "";
  el("tr-lasttag").hidden = true;
  if (showPrice) renderPriceChart();
  else if (!showTape) renderDepthChart();
}

function renderPriceChart(): void {
  const chart = el("tr-chart");
  const m = market;
  const empty = el("tr-chart-empty");
  // Oldest first.
  const points = [...trades].reverse();
  empty.textContent = "No trade has happened since this page connected. FlowMesh keeps no price history, so the chart starts here.";
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
  // Time runs along the chart: two trades a second apart sit together, and
  // the clock times under its ends mean what they say. Trades all seen at
  // one moment are spread evenly instead.
  const span = points[points.length - 1].at - points[0].at;
  const x = (i: number) =>
    points.length === 1 ? right / 2 : (span > 0 ? (points[i].at - points[0].at) / span : i / (points.length - 1)) * (right - 2 * pad) + pad;
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
  // The amounts traded, on their own scale below the prices.
  const most = points.reduce((a, t) => (t.lots > a ? t.lots : a), 0n);
  const barW = points.length > 40 ? 3 : 6;
  const room = H - PRICE_BOTTOM - 4;
  const bars = points.map((t, i) => {
    const tall = most > 0n ? (Number((t.lots * 1000n) / most) / 1000) * room : 0;
    return svg("rect", { class: "vol", x: (x(i) - barW / 2).toFixed(1), y: (H - tall).toFixed(1), width: barW.toFixed(1), height: Math.max(1, tall).toFixed(1) });
  });
  const dots = points.length <= 60 ? points.map((_, i) => svg("circle", { cx: x(i).toFixed(1), cy: y(prices[i]).toFixed(1), r: "2.5", fill: "var(--accent)" })) : [];
  chart.replaceChildren(defs, ...grid, ...bars, area, line, lastLine, ...dots);

  const lastPrice = prices[prices.length - 1];
  el("tr-chart-high").textContent = hi === lastPrice ? "" : price(hi, m);
  el("tr-chart-low").textContent = lo === lastPrice ? "" : price(lo, m);
  const tag = el("tr-lasttag");
  tag.textContent = price(prices[prices.length - 1], m);
  tag.style.top = `${((lastY / H) * 100).toFixed(2)}%`;
  tag.hidden = false;
  el("tr-chart-first").textContent = clock(points[0].at);
  el("tr-chart-mid").textContent = `${points.length} trade${points.length === 1 ? "" : "s"}, in ${priceUnit(m)}`;
  el("tr-chart-last").textContent = clock(points[points.length - 1].at);
  // The eye reads the line; this says the whole chart at once.
  chart.setAttribute(
    "aria-label",
    `Trade prices in ${priceUnit(m)}: ${points.length} trade${points.length === 1 ? "" : "s"}, last ${price(prices[prices.length - 1], m)}, high ${price(hi, m)}, low ${price(lo, m)}`,
  );
}

function renderDepthChart(): void {
  const chart = el("tr-depth");
  const m = market;
  const b = depth(book?.bids ?? []);
  const a = depth(book?.asks ?? []);
  const empty = el("tr-chart-empty");
  empty.textContent = book ? "No orders are waiting in this market." : "Waiting for the live view.";
  empty.hidden = !!m && (b.length > 0 || a.length > 0);
  if (!m || (!b.length && !a.length)) {
    chart.replaceChildren();
    chart.setAttribute("aria-label", "Book depth: no orders are waiting");
    return;
  }
  chart.setAttribute(
    "aria-label",
    `Book depth: ${b.length} price${b.length === 1 ? "" : "s"} with buyers${b.length ? `, the highest ${price(b[0].ticks, m)}` : ""}; ` +
      `${a.length} with sellers${a.length ? `, the lowest ${price(a[0].ticks, m)}` : ""}`,
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
  el("tr-chart-high").textContent = size(cMax, m);
  el("tr-chart-first").textContent = price(pLo, m);
  el("tr-chart-mid").textContent = `Buyers left, sellers right, in ${priceUnit(m)}`;
  el("tr-chart-last").textContent = price(pHi, m);
}

function renderTrades(): void {
  const m = market;
  const body = el("tr-trades-rows");
  el("tr-trades-price").textContent = m ? `Price (${unit(m.quote)})` : "Price";
  el("tr-trades-amount").textContent = m ? `Amount (${unit(m.base)})` : "Amount";
  el("tr-trades-value").textContent = m ? `Value (${unit(m.quote)})` : "Value";
  el("tr-trades-note").textContent = !m
    ? ""
    : trades.length
      ? "Green was a buyer taking a seller's price, red the other way round. From the live view."
      : links.get("trades")?.live
        ? "No trade has happened in this market since this page connected. FlowMesh keeps no list of earlier trades."
        : "Waiting for the live view.";
  if (!m) {
    body.replaceChildren();
    return;
  }
  body.replaceChildren(
    ...trades.map((t) => {
      const tr = h("tr");
      const yours = cell("");
      if (t.mine) yours.append(h("span", "badge badge-accent", t.mine === "maker" ? "Your waiting order" : "Your order"));
      tr.append(
        cell(clock(t.at), "tnum"),
        cell(price(t.ticks, m), `num tnum ${t.takerSide === "buy" ? "tr-up" : "tr-down"}`),
        cell(amount(baseAtoms(t.lots, m), m.base, lotPlaces(m)), "num tnum"),
        cell(amount(t.quote, m.quote), "num tnum"),
        yours,
      );
      return tr;
    }),
  );
}

// ---- your account ----------------------------------------------------------

function renderAccount(): void {
  if (!info) return;
  const a = account;
  const has = info.account !== null;
  const answered = !!a && a.seats.some((s) => s.height !== null);
  const proof = el("tr-account-proof");
  // Pro says as of when: a trader weighs a balance by its age.
  const confirmed = mode === "pro" && a?.height ? `Confirmed by the network, as of slot ${whole(BigInt(a.height))}` : "Confirmed by the network";
  proof.textContent = !has ? "" : !a ? "Reading your account" : a.proved ? confirmed : answered ? "" : "No validator answered";
  proof.hidden = proof.textContent === "";
  proof.title = a
    ? [
        a.proved ? `As the network confirmed it in slot ${a.height}. B3 Hive checked the proof.` : "",
        ...a.seats.map((s) => `Validator ${s.seat + 1}: ${s.error ?? (s.proved ? `proved it at slot ${s.height}` : `does not know the account (slot ${s.height})`)}`),
      ]
        .filter(Boolean)
        .join("\n")
    : "";
  el("tr-balances").replaceChildren(
    ...(a?.proved ? a.balances : []).map((b) => {
      const line = h("div", "tr-bal");
      const amounts = h("span", "tr-bal-amounts");
      const places = assetPlaces(b.asset, info?.markets ?? []);
      const free = h("span", "tr-bal-free", masked(amount(BigInt(b.available), b.asset, places)));
      free.title = "What you can trade or send right now.";
      amounts.append(free);
      if (BigInt(b.reserved) > 0n) {
        const set = h("span", "tr-bal-held", `plus ${masked(amount(BigInt(b.reserved), b.asset, places))} set aside for open orders`);
        set.title = "Set aside for your orders that are waiting. It comes back when an order is cancelled.";
        amounts.append(set);
      }
      line.append(hex(b.asset), h("span", "tr-bal-name", unit(b.asset)), amounts);
      return line;
    }),
  );
  const note = el("tr-account-note");
  note.hidden = !!a?.proved;
  note.textContent = !has
    ? "Your balances appear here once you have an account."
    : !a
      ? "Looking up your account."
      : a.proved
        ? ""
        : answered
          ? "No funds yet. They appear here by themselves as soon as the network confirms a deposit."
          : "No validator answered, so no balance is shown.";
  // Until there are funds, the steps beside this card hold the account
  // number and say how to get them.
  el("tr-account-actions").hidden = !ready();
  const send = el<HTMLButtonElement>("tr-send-open");
  send.disabled = pending() || !a?.balances.some((b) => BigInt(b.available) > 0n);
  send.title = pending() ? ONE_AT_A_TIME : "";
}

function renderOrders(): void {
  if (!info) return;
  const a = account;
  const orders = a?.orders ?? [];
  const count = el("tr-orders-count");
  count.hidden = orders.length === 0;
  count.textContent = String(orders.length);
  el("tr-lists").toggleAttribute("data-has-orders", orders.length > 0 || !!a?.orders_mismatch || flows.has("orders"));
  el("tr-orders-actions").hidden = orders.length < 2;
  const all = el<HTMLButtonElement>("tr-cancel-all");
  all.disabled = pending();
  all.title = pending() ? ONE_AT_A_TIME : "Every order this account has waiting, in every market of this network, with one approval.";
  const mismatch = el("tr-orders-mismatch");
  mismatch.hidden = !a?.orders_mismatch;
  mismatch.textContent = a?.orders_mismatch ? "This list may be out of date: an order may have traded while B3 Hive was not watching. Your balances are confirmed and correct." : "";
  mismatch.title = a?.orders_mismatch ?? "";
  el("tr-orders-empty").textContent = orders.length
    ? mode === "pro"
      ? "An order is listed once the network has confirmed it. What has traded since then is from the live view."
      : ""
    : "No orders are waiting. An order at a price of your own waits here until it trades or you cancel it.";
  if (mode === "pro" && orders.length) {
    el("tr-orders-rows").replaceChildren(ordersTable(orders));
    return;
  }
  el("tr-orders-rows").replaceChildren(
    ...orders.map((o) => {
      const m = info?.markets.find((x) => x.id === o.market_id);
      const buy = o.side === "buy";
      const item = h("div", "tr-row");
      const main = h("div", "tr-row-main");
      const lead = h("span", "tr-row-lead");
      lead.dataset.tone = o.side;
      lead.append(icon(buy ? "arrow-down" : "arrow-up", "i-sm"));
      const placed = BigInt(o.placed_lots);
      const left = BigInt(o.remaining_lots);
      const done = placed > left ? placed - left : 0n;
      const ticks = BigInt(o.price_ticks);
      const text = h("div", "tr-row-body");
      text.append(
        h("span", "tr-row-title", m ? `${buy ? "Buy" : "Sell"} ${size(placed, m)} at ${priceText(ticks, m)}` : `${buy ? "Buy" : "Sell"} in ${o.market}`),
        h(
          "span",
          "tr-row-meta",
          !m
            ? ""
            : done > 0n
              ? `${size(done, m)} ${buy ? "bought" : "sold"} so far, ${size(left, m)} to go`
              : buy
                ? "Waiting for a seller at that price"
                : "Waiting for a buyer at that price",
        ),
      );
      if (m) main.title = `Still set aside: ${masked(buy ? amountText(quoteAtoms(left, ticks, m), m.quote) : size(left, m))}\nIn the book since slot ${o.height}\nOrder ${o.order}`;
      const trail = h("div", "tr-row-trail");
      if (done > 0n && placed > 0n) {
        const part = Number((done * 100n) / placed);
        const fill = h("div", "tr-row-fill");
        const track = h("div", "progress");
        const bar = h("div", "progress-bar");
        bar.style.width = `${part}%`;
        track.append(bar);
        fill.append(track);
        fill.title = `${part}% done`;
        trail.append(fill);
      }
      const cancel = action(o.cancelling ? "Cancelling" : "Cancel", "btn btn-secondary btn-sm", () => void cancelOrder(o, m));
      cancel.disabled = o.cancelling || pending();
      if (o.cancelling) cancel.title = "The cancellation is signed and waits for the network. Until it is confirmed, the order can still trade.";
      else if (pending()) cancel.title = ONE_AT_A_TIME;
      trail.append(cancel);
      main.append(lead, text, trail);
      item.append(main);
      return item;
    }),
  );
}

/** The open orders as a trader reads them: one row of fields each. */
function ordersTable(orders: Order[]): HTMLElement {
  const several = new Set(orders.map((o) => o.market_id)).size > 1;
  const table = h("table", "tbl tbl-compact tr-orders-table");
  const head = h("tr");
  const th = (text: string, cls = "") => {
    const cell = h("th", cls, text);
    cell.scope = "col";
    return cell;
  };
  const placedHead = th("Placed");
  placedHead.title = "When you approved it, by this computer's clock.";
  head.append(placedHead, ...(several ? [th("Market")] : []), th("Side"), th("Price", "num"), th("Amount", "num"), th("Filled", "num"), th(""));
  const thead = h("thead");
  thead.append(head);
  const body = h("tbody");
  for (const o of orders) {
    const m = info?.markets.find((x) => x.id === o.market_id);
    const buy = o.side === "buy";
    const placed = BigInt(o.placed_lots);
    const left = BigInt(o.remaining_lots);
    const done = placed > left ? placed - left : 0n;
    const ticks = BigInt(o.price_ticks);
    const created = account?.activity.find((r) => r.label === o.label)?.created;
    const row = h("tr");
    // The history keeps seconds; an order from another day shows its date.
    const at = created ? new Date(created * 1000) : null;
    const when = cell(at ? (at.toDateString() === new Date().toDateString() ? clock(created! * 1000) : at.toLocaleDateString()) : "—", "muted tnum");
    when.title = `In the book since slot ${o.height}\nOrder ${o.order}`;
    const sideCell = cell(buy ? "Buy" : "Sell", buy ? "tr-up" : "tr-down");
    const filled = cell(m ? (done > 0n ? `${size(done, m)} (${Number((done * 100n) / placed)}%)` : "0%") : "", "num tnum");
    if (m && done > 0n) filled.title = `${size(done, m)} has traded and ${size(left, m)} still waits. From the live view.`;
    const act = h("td", "num");
    const cancel = action(o.cancelling ? "Cancelling" : "Cancel", "btn btn-secondary btn-sm", () => void cancelOrder(o, m));
    cancel.disabled = o.cancelling || pending();
    if (o.cancelling) cancel.title = "The cancellation is signed and waits for the network. Until it is confirmed, the order can still trade.";
    else if (pending()) cancel.title = ONE_AT_A_TIME;
    if (m) cancel.setAttribute("aria-label", `Cancel: ${buy ? "buy" : "sell"} ${size(placed, m)} at ${priceText(ticks, m)}`);
    act.append(cancel);
    row.append(
      when,
      ...(several ? [cell(o.market)] : []),
      sideCell,
      cell(m ? `${price(ticks, m)} ${unit(m.quote)}` : "", "num tnum"),
      cell(m ? masked(size(placed, m)) : "", "num tnum"),
      filled,
      act,
    );
    if (m) row.title = `Still set aside: ${masked(buy ? amountText(quoteAtoms(left, ticks, m), m.quote) : size(left, m))}`;
    body.append(row);
  }
  table.append(thead, body);
  const wrap = h("div", "table-wrap");
  wrap.append(table);
  return wrap;
}

function renderActivity(): void {
  if (!info) return;
  const rows = account?.activity ?? [];
  const waiting = rows.filter((r) => r.state === "open").length;
  const badge = el("tr-activity-count");
  badge.hidden = waiting === 0;
  badge.textContent = String(waiting);
  badge.title = `${waiting} still waiting for the network`;
  el("tr-activity-empty").textContent = rows.length ? "" : "Nothing yet. What you buy, sell, cancel or send appears here, with what became of it.";
  el("tr-activity-foot").hidden = rows.length <= historyShown && waiting === 0;
  el("tr-activity-more").hidden = rows.length <= historyShown;
  el("tr-activity-refresh").hidden = waiting === 0;
  // Redrawing must not close a row the user opened, or lose their place.
  const list = el("tr-activity-rows");
  const open = new Set([...list.querySelectorAll<HTMLDetailsElement>("details[open]")].map((d) => d.dataset.label));
  const focused = (document.activeElement as HTMLElement | null)?.closest<HTMLElement>("#tr-activity-rows details")?.dataset.label;
  const today = new Date().toDateString();
  list.replaceChildren(
    ...rows.slice(0, historyShown).map((r) => {
      const told = tell(r, standing);
      const item = h("details", "tr-row");
      item.dataset.label = r.label;
      item.open = open.has(r.label);
      const main = h("summary", "tr-row-main");
      const lead = h("span", "tr-row-lead");
      lead.dataset.tone = told.tone;
      lead.append(icon(told.icon, "i-sm"));
      const text = h("div", "tr-row-body");
      text.append(h("span", "tr-row-title", told.title));
      if (told.meta) text.append(h("span", "tr-row-meta", told.meta));
      const when = new Date(r.created * 1000);
      const trail = h("div", "tr-row-trail");
      const time = h("span", "t-xs tnum", when.toDateString() === today ? clock(r.created * 1000).slice(0, 5) : when.toLocaleDateString());
      time.title = when.toLocaleString();
      trail.append(time, icon("chevron-right", "i-sm caret"));
      main.append(lead, text, trail);

      const more = h("div", "tr-row-more");
      if (told.detail) more.append(h("p", "", told.detail));
      const facts = h("div", "details");
      const fact = (key: string, value: string, mono = false, title = "") => {
        const line = h("div", "drow");
        const shownValue = h("span", mono ? "dval mono" : "dval", value);
        if (title) shownValue.title = title;
        line.append(h("span", "dkey", key), shownValue);
        facts.append(line);
      };
      fact("Signed", when.toLocaleString());
      if (r.state === "included") fact("Confirmed in", `slot ${whole(BigInt(r.height))}`, false, `${outcomeText(r.code_name)} (code ${r.code}), action ${r.index} of that slot. B3 Hive checked the proof.`);
      else if (r.state === "expired") fact("Proved expired at", `slot ${whole(BigInt(r.height))}`, false, "The validators proved that no slot can include it any more.");
      else fact("Valid until", `slot ${whole(BigInt(r.ref_slot) + BigInt(r.window_slots))}`, false, "After that the validators can prove it expired, and it can never happen.");
      if (r.intent.action === "place") {
        fact("Time in force", tifText(r.intent.tif));
        fact("Price limit", priceText(BigInt(r.intent.price_ticks), r.intent.market));
        fact(
          "In lots and ticks",
          `${whole(BigInt(r.intent.lots))} lots at ${whole(BigInt(r.intent.price_ticks))} ticks`,
          false,
          `One lot is ${lotText(r.intent.market)}. One tick moves the price by ${tickText(r.intent.market)}.`,
        );
      }
      fact("Nonce", r.nonce, false, "This account's counter in the action, so that it can only ever happen once.");
      fact("Action id", r.action_id, true);
      // The trades behind the sentence, one row each, as the network
      // confirmed them.
      if (r.state === "included" && r.intent.action === "place" && r.fills.length) more.append(fillsTable(r.fills, r.intent.market, r.intent.side === "sell"));
      more.append(facts);
      if (r.state === "open") {
        const said = h("p", "muted", r.note || "No validator has answered about it yet.");
        const again = action("Send it again", "btn btn-secondary btn-sm", () => void postAgain(r.label, again, said));
        again.title = "Sends the same signed action to every validator again. Nothing new is signed.";
        const buttons = h("div", "btn-row");
        buttons.append(again);
        more.append(said, buttons);
      }
      item.append(main, more);
      return item;
    }),
  );
  if (focused) list.querySelector<HTMLElement>(`details[data-label="${CSS.escape(focused)}"] > summary`)?.focus();
}

/** The trades a confirmed order made, one row each: what the one sentence
 *  about it adds up. */
function fillsTable(fills: Fill[], m: Market, sold: boolean): HTMLElement {
  const table = h("table", "tbl tbl-compact tr-fills");
  const head = h("tr");
  const columns: [string, string][] = [
    [sold ? "Sold" : "Bought", ""],
    ["Price", "num"],
    ["Value", "num"],
  ];
  if (sold) columns.push(["Fee", "num"]);
  for (const [text, cls] of columns) {
    const th = h("th", cls, text);
    th.scope = "col";
    if (text === "Fee") th.title = "The seller's fee is rounded up once for the whole order, so its parts do not each round up.";
    head.append(th);
  }
  const thead = h("thead");
  thead.append(head);
  const body = h("tbody");
  for (const f of fills) {
    const row = h("tr");
    row.append(
      cell(size(BigInt(f.lots), m), "tnum"),
      cell(`${price(BigInt(f.price_ticks), m)} ${unit(m.quote)}`, "num tnum"),
      cell(amountText(BigInt(f.quote.atoms), m.quote), "num tnum"),
    );
    if (sold) row.append(cell(amountText(BigInt(f.fee_atoms ?? "0"), m.quote), "num tnum"));
    body.append(row);
  }
  table.append(thead, body);
  const wrap = h("div", "table-wrap");
  wrap.append(table);
  // One order pays one fee, rounded up once. Per trade it shows where the
  // rounding fell, which can look like a higher rate on one of them.
  if (sold && info) wrap.append(h("p", "t-xs muted tr-fills-note", `The seller's fee is ${feeRate(info.fee_ppm)} of what the whole order brings, rounded up once for the order, not for each trade.`));
  return wrap;
}

async function cancelOrder(o: Order, m: Market | undefined): Promise<void> {
  // Named as it was placed, with what is left of it when part has traded.
  const placed = BigInt(o.placed_lots);
  const left = BigInt(o.remaining_lots);
  const what = m ? `the order to ${o.side} ${size(placed, m)}${left < placed ? `, of which ${size(left, m)} still waits` : ""}` : "the order";
  await offer("orders", `Cancelling ${what}`, () => invoke<string>("fm2_cancel_draft", { order: o.order }), "Keep the order");
}

async function cancelAll(): Promise<void> {
  await offer("orders", "Cancelling all orders", () => invoke<string>("fm2_cancel_all_draft"), "Keep the orders");
}

async function postAgain(label: string, button: HTMLButtonElement, said: HTMLElement): Promise<void> {
  button.disabled = true;
  try {
    const admissions = await invoke<Admission[]>("fm2_resend", { label });
    const took = admissions.filter((a) => a.accepted).length;
    const others = [...new Set(admissions.filter((a) => !a.accepted).map((a) => a.reason))].join("; ");
    said.textContent = took === admissions.length ? `All ${took} validators have it.` : `${took} of ${admissions.length} validators have it. The others said: ${others}`;
    void invoke("fm2_refresh").catch(() => {});
  } catch (e) {
    said.textContent = String(e);
  } finally {
    button.disabled = false;
  }
}

// ---- Pro: the order ticket -------------------------------------------------

/** What the ticket would order right now. */
type Ticket = {
  m: Market;
  buy: boolean;
  have: bigint;
  ticks: bigint | null;
  lots: bigint | null;
  /** The plan of a market order. */
  p: Plan | null;
  error: string;
  warnings: string[];
  ok: boolean;
};

function ticket(): Ticket | null {
  const m = market;
  if (!m || !info) return null;
  const buy = side === "buy";
  const spend = buy ? m.quote : m.base;
  const have = available(spend) ?? 0n;
  const t: Ticket = { m, buy, have, ticks: null, lots: null, p: null, error: "", warnings: [], ok: false };
  const amountTyped = el<HTMLInputElement>("tr-amount").value.trim();
  if (amountTyped !== "") {
    const atoms = parseAmount(amountTyped, m.base);
    if (atoms === null) t.error = numberProblem(amountTyped, m.base);
    else if (atoms === 0n || atoms % BigInt(m.base_atoms_per_lot) !== 0n) t.error = `Amounts move in steps of ${lotText(m)}.`;
    else t.lots = lotsIn(atoms, m);
  }
  if (kind === "market") {
    const offers = book ? (buy ? book.asks : book.bids) : [];
    if (t.lots !== null && book) {
      const p = plan(side, { of: "base", atoms: baseAtoms(t.lots, m) }, offers, have, m, info.fee_ppm);
      t.p = p;
      if (!offers.length) t.error = buy ? "Nobody is selling right now." : "Nobody is buying right now.";
      else if (p.short === "book") t.error = buy ? `Only ${size(p.offered, m)} is for sale right now.` : `Buyers want only ${size(p.offered, m)} right now.`;
      else if (p.short === "funds") t.error = `You have ${amountText(have, spend)} available. That is enough for ${size(p.lots, m)}.`;
      else {
        t.ticks = p.limit;
        if (meetsOwn(m, buy, p.limit)) t.warnings.push(OWN_ORDER);
        const far = reach(p);
        if (far >= FAR_BPS) {
          const beyond = p.lots - BigInt(offers[0].lots);
          t.warnings.push(
            buy
              ? `${size(beyond, m)} of it costs more than the best price, up to ${priceText(p.limit, m)}: ${percent(far)} above it.`
              : `${size(beyond, m)} of it sells below the best price, down to ${priceText(p.limit, m)}: ${percent(far)} below it.`,
          );
        }
      }
    }
    t.ok = !t.error && !!t.p && t.p.lots > 0n && t.p.short === null && !!links.get("book")?.live;
    return t;
  }
  const priceTyped = el<HTMLInputElement>("tr-price").value.trim();
  if (priceTyped !== "") {
    t.ticks = parsePrice(priceTyped, m);
    if (t.ticks === null && !t.error) t.error = nearestPrice(priceTyped, m) === null ? numberProblem(priceTyped, m.quote) : `Prices move in steps of ${tickText(m)}.`;
  }
  const tif = stay;
  if (!t.error && t.ticks !== null && t.lots !== null) {
    const need = buy ? quoteAtoms(t.lots, t.ticks, m) : baseAtoms(t.lots, m);
    const waiting = account?.open_orders ?? null;
    if (need > have) t.error = `This needs ${amountText(need, spend)}. You have ${amountText(have, spend)} available.`;
    else if (tif !== "ioc" && waiting !== null && waiting >= info.max_open_orders) {
      t.error = `This account already has ${info.max_open_orders} orders waiting, the most the network allows.`;
    }
    const best = book ? (buy ? book.asks[0] : book.bids[0]) : undefined;
    if (best) {
      const top = BigInt(best.price_ticks);
      const crosses = buy ? t.ticks >= top : t.ticks <= top;
      // A post-only order that crosses is refused: the line about right
      // now says so, and what price it needs.
      if (crosses && tif !== "post_only") {
        const gap = buy ? t.ticks - top : top - t.ticks;
        const far = (gap * 10_000n) / top;
        if (far >= THROUGH_BPS) t.warnings.push(`Your price is ${percent(far)} ${buy ? "above the lowest ask" : "below the highest bid"}. It trades at the book's prices, up to yours.`);
        if (meetsOwn(m, buy, t.ticks)) t.warnings.push(OWN_ORDER);
      }
    }
  }
  t.ok = !t.error && t.ticks !== null && t.lots !== null;
  return t;
}

function renderTicket(): void {
  const t = ticket();
  if (!t || !info || !ready()) return;
  const { m, buy, have } = t;
  const atOnce = kind === "market";
  el("tr-ticket").dataset.side = side;
  el("tr-buy").setAttribute("aria-selected", String(buy));
  el("tr-sell").setAttribute("aria-selected", String(!buy));
  el("tr-kind-limit").setAttribute("aria-selected", String(!atOnce));
  el("tr-kind-market").setAttribute("aria-selected", String(atOnce));
  for (const id of ["tr-price-row", "tr-total-row", "tr-tif-row"]) el(id).hidden = atOnce;
  el("tr-price-unit").textContent = unit(m.quote);
  el("tr-amount-unit").textContent = unit(m.base);
  el("tr-total-unit").textContent = unit(m.quote);
  const spend = buy ? m.quote : m.base;
  el("tr-avbl").textContent = masked(held(have, spend));

  // The total follows price and amount, unless the user is typing it.
  const total = el<HTMLInputElement>("tr-total");
  if (!atOnce && !totalLed && document.activeElement !== total) {
    total.value = t.ticks !== null && t.lots !== null ? amount(quoteAtoms(t.lots, t.ticks, m), m.quote) : "";
  }

  for (const button of document.querySelectorAll<HTMLButtonElement>("#tr-tif [data-tif]")) button.setAttribute("aria-pressed", String(button.dataset.tif === stay));
  // Away from the keyboard, a number reads in groups like every other on
  // the page, so a zero too many shows.
  const grouped = (id: string, text: string | null) => {
    const input = el<HTMLInputElement>(id);
    if (text !== null && document.activeElement !== input && input.value.trim() !== "" && input.value !== text) input.value = text;
  };
  grouped("tr-price", !atOnce && t.ticks !== null ? price(t.ticks, m) : null);
  grouped("tr-amount", t.lots !== null ? amount(baseAtoms(t.lots, m), m.base) : null);

  // The rule behind the fee's amount stands with it, a pointer away.
  const feeRule = `${feeRate(info.fee_ppm)} of what the whole order brings, rounded up once to the smallest unit of ${unit(m.quote)}.`;
  const feeLine = (value: string): HTMLDivElement => {
    const line = pair("Seller's fee", value);
    line.title = feeRule;
    return line;
  };
  // A sale's floor holds for the whole amount: less sold brings less.
  const LEAST = "For all of it you receive at least";
  let lines: HTMLElement[] = [];
  // The order as typed would be refused as the book stands.
  let refused = false;
  if (atOnce && t.p && t.p.lots > 0n && !t.error) {
    const sure = t.p.limit === t.p.best;
    // The engine has no market order: say what is sent in its place, where
    // its limit comes from, and what becomes of what cannot trade.
    const sent = h("div", "tr-sum-now");
    sent.textContent = `Sent as a limit order at ${priceText(t.p.limit, m)}, the ${buy ? "highest" : "lowest"} price it needs in the book right now. What cannot trade at once is dropped.`;
    sent.title = "For a limit of your own, use Limit with \"Is dropped IOC\".";
    lines.push(sent);
    if (!sure) lines.push(pair("Average price", `about ${averageText(t.p.gross, t.p.lots, m)}`));
    if (buy) {
      lines.push(
        pair(sure ? "You pay" : "You pay about", amountText(sure ? t.p.ceiling : t.p.gross, m.quote)),
        pair("You pay at most", amountText(t.p.ceiling, m.quote)),
        pair("Fee", "None for buying"),
      );
    } else {
      const worst = quoteAtoms(t.p.lots, t.p.limit, m);
      lines.push(
        pair(sure ? "You receive" : "You receive about", amountText(t.p.gross - t.p.fee, m.quote)),
        pair(LEAST, amountText(worst - sellerFee(worst, info.fee_ppm), m.quote)),
        feeLine(`${amountText(t.p.fee, m.quote)}, already subtracted above`),
      );
    }
  } else if (!atOnce && t.ticks !== null && t.lots !== null) {
    const quote = quoteAtoms(t.lots, t.ticks, m);
    const fee = sellerFee(quote, info.fee_ppm);
    // What it would do against the book as it stands, then what its own
    // price guarantees.
    const now = h("div", "tr-sum-now");
    const said = book && links.get("book")?.live ? outlook(side, stay, t.ticks, t.lots, buy ? book.asks : book.bids, m) : null;
    now.textContent = said ? said.text : "The live view is not arriving, so what it would do right now is unknown.";
    if (!said || said.refused) now.classList.add("tr-sum-warn");
    now.title = "From the live view: an estimate.";
    lines.push(now);
    refused = !!said?.refused && !t.error;
    // An order that would be refused promises nothing: no bounds under it.
    if (refused) {
      // (the line above says why, and what price it needs)
    } else if (buy) {
      const most = pair("You pay at most", amountText(quote, m.quote));
      if (stay !== "ioc") most.title = "Set aside from your balance while the order waits.";
      lines.push(most, pair("Fee", "None for buying"));
    } else {
      lines.push(pair(LEAST, amountText(quote - fee, m.quote)), feeLine(`${amountText(fee, m.quote)} at this price, already subtracted above`));
    }
  }
  // While a request from this form is under way, its steps stand where the
  // summary was: above the button, on the first screen. Its result stays
  // there until the form is changed: from then on the form is about the
  // next order, and the result is in the history.
  const last = flows.get("trade");
  if (mode === "pro" && last?.stage === "done" && last.form !== ticketState(t)) {
    flows.delete("trade");
    renderFlow("trade");
  }
  const flowing = flows.has("trade");
  if (flowing) lines = [];
  el("tr-summary").replaceChildren(...lines);

  const errorBox = el("tr-form-error");
  errorBox.hidden = t.error === "" || flowing;
  errorBox.textContent = t.error;
  banner("tr-form-warn", "warning", t.ok && !refused && !flowing ? t.warnings.join(" ") : "", "tr-inline");

  const button = el<HTMLButtonElement>("tr-review");
  const verb = buy ? "Buy" : "Sell";
  const lots = atOnce ? (t.p?.lots ?? null) : t.lots;
  const stopped = network().state === "bad";
  button.textContent = pending()
    ? ownWaits()
      ? WAITING.own
      : WAITING.other
    : stopped
      ? "Waiting for the network"
      : atOnce && !links.get("book")?.live
        ? "Waiting for live prices"
        : refused
          ? "Would be refused right now"
          : t.ok && lots
          ? atOnce && t.p
            ? // The same words as the simple card's button: what it costs
              // or brings, "about" when it reaches past the best price.
              `${verb} ${size(lots, m)} for ${money(t.p, buy, m)}`
            : `${verb} ${size(lots, m)} at ${price(t.ticks ?? 0n, m)} ${unit(m.quote)}`
          : t.error
            ? // The line above the button says what is wrong.
              `${verb} ${unit(m.base)}`
            : atOnce
              ? "Enter an amount"
              : "Enter a price and an amount";
  button.disabled = !t.ok || refused || pending() || stopped;

  // The slider is a share of what the available balance pays for.
  el("tr-slider").title = atOnce
    ? "A share of the most that can trade at once: what you have available, or everything on offer if that is less."
    : "A share of what you have available.";
  const { lots: most, byBook } = mostNow(t);
  // When the book and not the balance sets the most, 100% is less than
  // "Available" says, and the form says what it is.
  const note = el("tr-slider-note");
  note.hidden = !(atOnce && byBook);
  note.textContent = note.hidden ? "" : `100% is ${size(most, m)}: all that ${buy ? "is for sale" : "buyers want"} right now.`;
  const slider = el<HTMLInputElement>("tr-pct");
  slider.disabled = most === 0n;
  const pct = most > 0n && lots ? Math.min(100, Number((lots * 100n) / most)) : 0;
  if (document.activeElement !== slider) slider.value = String(pct);
  el("tr-slider").style.setProperty("--pct", `${pct}%`);
  // A share too small to round to one percent is still not nothing.
  el("tr-pct-out").textContent = pct === 0 && most > 0n && lots ? "<1%" : `${pct}%`;
  for (const mark of document.querySelectorAll<HTMLElement>("#tr-slider [data-pct]")) mark.classList.toggle("on", Number(mark.dataset.pct) <= pct && pct > 0);
}

/** The most the ticket can order with what is available, and whether what
 *  is on offer, not the balance, sets it. */
function mostNow(t: Ticket): { lots: bigint; byBook: boolean } {
  if (!info) return { lots: 0n, byBook: false };
  if (kind === "limit") return { lots: t.buy ? (t.ticks !== null ? maxLots("buy", t.have, t.ticks, t.m) : 0n) : maxLots("sell", t.have, 0n, t.m), byBook: false };
  if (!book) return { lots: 0n, byBook: false };
  const offers = t.buy ? book.asks : book.bids;
  const p = plan(side, t.buy ? { of: "quote", atoms: t.have } : { of: "base", atoms: t.have }, offers, t.have, t.m, info.fee_ppm);
  return { lots: p.lots, byBook: p.lots > 0n && p.short === "book" };
}

function setPart(pct: number): void {
  const t = ticket();
  if (!t) return;
  totalLed = false;
  const lots = (mostNow(t).lots * BigInt(Math.round(pct))) / 100n;
  el<HTMLInputElement>("tr-amount").value = lots > 0n ? plain(baseAtoms(lots, t.m), t.m.base) : "";
  renderTicket();
}

/** Steps the price by one tick, or the amount by one lot. */
function nudge(id: string, dir: number): void {
  const m = market;
  if (!m) return;
  const input = el<HTMLInputElement>(id);
  totalLed = false;
  if (id === "tr-price") {
    const typed = nearestPrice(input.value.trim(), m);
    // From an empty field, the first step lands on the best price.
    const best = book ? (side === "buy" ? book.asks[0] : book.bids[0]) : undefined;
    const from = typed ?? (best ? BigInt(best.price_ticks) - BigInt(dir) : 0n);
    const next = from + BigInt(dir);
    if (next >= 1n) input.value = plainPrice(next, m);
  } else {
    const atoms = parseAmount(input.value.trim(), m.base);
    const next = (atoms === null ? 0n : lotsIn(atoms, m)) + BigInt(dir);
    if (next >= 1n) input.value = plain(baseAtoms(next, m), m.base);
  }
  renderTicket();
}

/** Takes a price from the book or the market strip into the ticket. */
function pickPrice(ticks: bigint): void {
  const m = market;
  if (!m) return;
  kind = "limit";
  totalLed = false;
  el<HTMLInputElement>("tr-price").value = plainPrice(ticks, m);
  renderTicket();
  if (mode === "pro" && ready()) el("tr-amount").focus();
}

/** A typed total sets the amount: the whole steps it pays for at the price. */
function totalTyped(): void {
  const m = market;
  if (!m) return;
  totalLed = true;
  const ticks = parsePrice(el<HTMLInputElement>("tr-price").value.trim(), m);
  const atoms = parseAmount(el<HTMLInputElement>("tr-total").value.trim(), m.quote);
  if (ticks !== null && atoms !== null) {
    const lots = atoms / quoteAtoms(1n, ticks, m);
    el<HTMLInputElement>("tr-amount").value = lots > 0n ? plain(baseAtoms(lots, m), m.base) : "";
  }
  renderTicket();
}

/** Leaving a field settles what was typed onto the market's steps. */
function settle(id: string): void {
  const m = market;
  if (!m) return;
  const input = el<HTMLInputElement>(id);
  const typed = input.value.trim();
  if (id === "tr-total") totalLed = false;
  else if (typed !== "" && id === "tr-price") {
    const ticks = nearestPrice(typed, m);
    if (ticks !== null) input.value = plainPrice(ticks, m);
  } else if (typed !== "") {
    const atoms = parseAmount(typed, m.base);
    if (atoms !== null && lotsIn(atoms, m) > 0n) input.value = plain(baseAtoms(lotsIn(atoms, m), m), m.base);
  }
  renderTicket();
}

async function reviewOrder(): Promise<void> {
  const t = ticket();
  if (!t?.ok) return;
  const { m, buy } = t;
  const atOnce = kind === "market";
  const lots = atOnce ? t.p?.lots : t.lots;
  const ticks = atOnce ? t.p?.limit : t.ticks;
  if (!lots || !ticks) return;
  const tif = atOnce ? "ioc" : stay;
  const verb = tif === "ioc" ? `${buy ? "Buying" : "Selling"} ${size(lots, m)}` : `Placing an order to ${side} ${size(lots, m)}`;
  const back = tif !== "ioc" ? "Don't place it" : buy ? "Don't buy" : "Don't sell";
  await offer("trade", verb, () => invoke<string>("fm2_place_draft", { order: { market: m.id, side, tif, price_ticks: ticks.toString(), lots: lots.toString() } }), back);
}

// ---- dialogs: add funds, send ----------------------------------------------

let dialog: { backdrop: HTMLElement; release: () => void; back: HTMLElement | null } | null = null;

function openDialog(id: string, focus: string): void {
  closeDialog();
  const backdrop = el(id);
  const back = document.activeElement as HTMLElement | null;
  backdrop.hidden = false;
  dialog = { backdrop, release: trapDialog(backdrop), back };
  el(focus).focus();
}

function closeDialog(): void {
  if (!dialog) return;
  dialog.backdrop.hidden = true;
  dialog.release();
  dialog.back?.focus();
  dialog = null;
}

/** The network's assets as a list in a sentence: "B3, TOK and rUSD". */
function assetNames(): string {
  const names = (info?.assets ?? []).map((a) => unit(a));
  return names.length > 1 ? `${names.slice(0, -1).join(", ")} and ${names[names.length - 1]}` : (names[0] ?? "");
}

function openFunds(): void {
  if (!info?.account) return;
  const listed = assetNames();
  el("tr-funds-assets").textContent = listed ? `This network has ${listed}. ` : "";
  el("tr-funds-account").textContent = info.account;
  el("tr-faucet").textContent = faucet();
  openDialog("tr-funds-backdrop", "tr-funds-copy");
}

function sendAsset(): Asset | null {
  const id = el<HTMLSelectElement>("tr-send-asset").value;
  return info?.assets.find((a) => a.id === id) ?? null;
}

function openSend(): void {
  if (!info || !account?.proved) return;
  const held = account.balances.filter((b) => BigInt(b.available) > 0n).map((b) => b.asset);
  options(
    el<HTMLSelectElement>("tr-send-asset"),
    held.map((a) => ({ id: a.id, text: unit(a) })),
    held[0]?.id ?? "",
  );
  for (const id of ["tr-send-to", "tr-send-amount"]) el<HTMLInputElement>(id).value = "";
  renderSend();
  openDialog("tr-send-backdrop", "tr-send-to");
}

function renderSend(): void {
  const asset = sendAsset();
  if (!asset) return;
  el("tr-send-unit").textContent = unit(asset);
  const have = available(asset) ?? 0n;
  el("tr-send-avbl").textContent = `You have ${masked(amountText(have, asset))}`;
  const to = el<HTMLInputElement>("tr-send-to").value.trim();
  const text = el<HTMLInputElement>("tr-send-amount").value.trim();
  const atoms = text === "" ? null : parseAmount(text, asset);
  let toError = "";
  if (to !== "" && !/^[0-9a-fA-F]{64}$/.test(to)) {
    toError = `An account number is 64 letters and digits (0 to 9, a to f). This one has ${to.length} characters${/^[0-9a-fA-F]*$/.test(to) ? "" : ", and some that do not belong"}.`;
  } else if (to !== "" && to.toLowerCase() === info?.account) toError = "That is your own account.";
  let error = "";
  if (text !== "" && atoms === null) error = numberProblem(text, asset);
  else if (atoms !== null && atoms > have) error = `That is more than you have.`;
  for (const [id, said] of [
    ["tr-send-to-error", toError],
    ["tr-send-error", error],
  ] as const) {
    el(id).hidden = said === "";
    el(id).textContent = said;
  }
  const ok = toError === "" && error === "" && to !== "" && atoms !== null && atoms > 0n;
  const button = el<HTMLButtonElement>("tr-send-review");
  // The button says what it does, in the words the approval window asks in.
  button.textContent = ok && atoms !== null ? `Send ${amountText(atoms, asset)}` : to === "" ? "Enter an account number" : "Enter an amount";
  button.disabled = !ok || pending();
}

async function reviewSend(): Promise<void> {
  const asset = sendAsset();
  if (!asset) return;
  const atoms = parseAmount(el<HTMLInputElement>("tr-send-amount").value.trim(), asset);
  const to = el<HTMLInputElement>("tr-send-to").value.trim().toLowerCase();
  if (atoms === null || atoms === 0n) return;
  closeDialog();
  await offer("account", `Sending ${amountText(atoms, asset)}`, () => invoke<string>("fm2_transfer_draft", { to, asset: asset.id, amount: atoms.toString() }), "Don't send");
}

// ---- wiring ----------------------------------------------------------------

type BookEvent = { seat: number; market: string; slot: string; bids: Level[]; asks: Level[] };
type TradesEvent = { seat: number; market: string; slot: string; fills: { taker_side: "buy" | "sell"; price_ticks: string; lots: string; quote_atoms: string; mine: "maker" | "taker" | null }[] };

function selectTab(tab: "orders" | "activity"): void {
  el("tr-lists").dataset.tab = tab;
  for (const name of ["orders", "activity"]) el(`tr-tab-${name}`).setAttribute("aria-selected", String(name === tab));
}

async function wire(): Promise<void> {
  await listen<{ channel: string; seat: number; live: boolean; detail: string }>("fm2-link", (event) => {
    links.set(event.payload.channel, event.payload);
    if (!active || !info) return;
    renderBar();
    if (event.payload.channel === "book") renderBook();
    if (event.payload.channel === "trades") renderTrades();
    renderSwap();
    renderTicket();
  });
  await listen<{ seat: number; height: string }>("fm2-slot", (event) => {
    slot = event.payload;
    // Only the open panel shows it.
    if (active && info && el<HTMLDetailsElement>("tr-net").open) renderBar();
  });
  await listen<BookEvent>("fm2-book", (event) => {
    if (event.payload.market !== market?.id) return;
    book = event.payload;
    if (!active) return;
    renderBook();
    renderStrip();
    renderSwap();
    renderTicket();
    if (chartView === "depth") renderChart();
  });
  await listen<TradesEvent>("fm2-trades", (event) => {
    const p = event.payload;
    if (p.market !== market?.id || p.fills.length === 0) return;
    const now = Date.now();
    const fresh = p.fills.map((f) => ({ at: now, slot: p.slot, takerSide: f.taker_side, ticks: BigInt(f.price_ticks), lots: BigInt(f.lots), quote: BigInt(f.quote_atoms), mine: f.mine }));
    // Newest first, the last fill of the slot on top.
    trades = [...fresh.reverse(), ...trades].slice(0, TRADES_KEPT);
    if (fresh.some((t) => t.mine)) void refreshAccount();
    if (!active) return;
    renderTrades();
    renderStrip();
    if (chartView === "price") renderChart();
    if (book && market) el("tr-mid-last").textContent = price(trades[0].ticks, market);
  });
  await listen("fm2-account", () => void refreshAccount());
  await listen("fm2-activity", () => void refreshAccount());

  // Connecting.
  el("tr-folder-pick").addEventListener("click", async () => {
    const folder = await invoke<string | null>("fm2_pick", { what: "folder" }).catch(() => null);
    if (!folder) return;
    banner("tr-connect-error", "danger", "");
    try {
      await connect(await invoke<Files>("fm2_locate", { folder }), false);
    } catch (e) {
      banner("tr-connect-error", "danger", String(e));
    }
  });
  el("tr-connect-form").addEventListener("submit", (e) => {
    e.preventDefault();
    void connect({ genesis: el<HTMLInputElement>("tr-genesis").value, pins: el<HTMLInputElement>("tr-pins").value }, false);
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

  // The network panel closes when the user clicks away or presses Escape.
  document.addEventListener("click", (e) => {
    const panel = el<HTMLDetailsElement>("tr-net");
    if (panel.open && !panel.contains(e.target as Node)) panel.open = false;
  });
  document.addEventListener("keydown", (e) => {
    if (e.key !== "Escape" || !active) return;
    if (dialog) closeDialog();
    else el<HTMLDetailsElement>("tr-net").open = false;
  });
  el("tr-net").addEventListener("toggle", () => {
    if (el<HTMLDetailsElement>("tr-net").open) renderBar();
  });

  el("tr-mode-simple").addEventListener("click", () => setMode("simple", true));
  el("tr-mode-pro").addEventListener("click", () => setMode("pro", true));

  // Simple.
  for (const [id, which] of [
    ["tr-pay", "pay"],
    ["tr-get", "get"],
  ] as const) {
    el(id).addEventListener("input", () => {
      driver = which;
      afterReject = false;
      renderSwap();
    });
  }
  el("tr-pay-asset").addEventListener("change", () => {
    const m = market;
    const pay = el<HTMLSelectElement>("tr-pay-asset").value;
    const asset = info?.assets.find((a) => a.id === pay);
    if (!asset || !m) return;
    const wasPay = side === "buy" ? m.quote.id : m.base.id;
    const wasGet = side === "buy" ? m.base.id : m.quote.id;
    const others = counterparts(asset);
    // What is received stays when it trades against the new choice;
    // choosing what was received turns the trade around.
    const get = others.find((a) => a.id === wasGet) ?? others.find((a) => a.id === wasPay) ?? others[0];
    if (get) setPair(pay, get.id);
  });
  el("tr-get-asset").addEventListener("change", () => setPair(el<HTMLSelectElement>("tr-pay-asset").value, el<HTMLSelectElement>("tr-get-asset").value));
  el("tr-flip").addEventListener("click", () => {
    el("tr-flip").classList.toggle("is-turned");
    setPair(el<HTMLSelectElement>("tr-get-asset").value, el<HTMLSelectElement>("tr-pay-asset").value);
  });
  for (const chip of document.querySelectorAll<HTMLElement>("#tr-swap .tr-chip")) {
    chip.addEventListener("click", () => share(BigInt(chip.dataset.share ?? "0")));
  }
  for (const [id, s] of [
    ["tr-swap-buy", "buy"],
    ["tr-swap-sell", "sell"],
  ] as const) {
    el(id).addEventListener("click", () => {
      const m = market;
      if (!m || side === s) return;
      if (s === "buy") setPair(m.quote.id, m.base.id);
      else setPair(m.base.id, m.quote.id);
    });
  }
  el("tr-swap-form").addEventListener("submit", (e) => {
    e.preventDefault();
    void reviewSwap();
  });

  // Lists.
  for (const tab of ["orders", "activity"] as const) el(`tr-tab-${tab}`).addEventListener("click", () => selectTab(tab));
  el("tr-cancel-all").addEventListener("click", () => void cancelAll());
  el("tr-activity-more").addEventListener("click", () => {
    historyShown += 20;
    renderActivity();
  });
  el("tr-activity-refresh").addEventListener("click", async () => {
    const button = el<HTMLButtonElement>("tr-activity-refresh");
    button.disabled = true;
    await invoke("fm2_refresh").catch((e) => banner("tr-error", "danger", String(e)));
    await refreshAccount();
    button.disabled = false;
  });

  // Pro.
  el<HTMLSelectElement>("tr-market").addEventListener("change", (e) => void selectMarket((e.target as HTMLSelectElement).value));
  for (const id of ["tr-best-bid", "tr-best-ask"]) {
    el(id).addEventListener("click", () => {
      const level = id === "tr-best-bid" ? book?.bids[0] : book?.asks[0];
      if (level) pickPrice(BigInt(level.price_ticks));
    });
  }
  for (const [id, view] of [
    ["tr-view-price", "price"],
    ["tr-view-depth", "depth"],
    ["tr-view-trades", "trades"],
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
  for (const [id, s] of [
    ["tr-buy", "buy"],
    ["tr-sell", "sell"],
  ] as const) {
    el(id).addEventListener("click", () => {
      if (side === s) return;
      side = s;
      // The simple card's two boxes change their meaning with the side; the
      // ticket's price and amount do not.
      for (const box of ["tr-pay", "tr-get"]) el<HTMLInputElement>(box).value = "";
      driver = "pay";
      syncPair();
      renderTicket();
      renderSwap();
    });
  }
  for (const [id, k] of [
    ["tr-kind-limit", "limit"],
    ["tr-kind-market", "market"],
  ] as const) {
    el(id).addEventListener("click", () => {
      kind = k;
      totalLed = false;
      renderTicket();
    });
  }
  for (const button of document.querySelectorAll<HTMLElement>("#tr-ticket .tr-stepper")) {
    button.addEventListener("click", () => nudge(button.dataset.for ?? "", Number(button.dataset.dir)));
  }
  // The arrow keys step a price or an amount, as the steppers do with a click.
  for (const id of ["tr-price", "tr-amount"]) {
    const input = el<HTMLInputElement>(id);
    input.setAttribute("aria-keyshortcuts", "ArrowUp ArrowDown");
    input.addEventListener("keydown", (e) => {
      if (e.key !== "ArrowUp" && e.key !== "ArrowDown") return;
      e.preventDefault();
      nudge(id, e.key === "ArrowUp" ? 1 : -1);
    });
    input.addEventListener("input", () => {
      totalLed = false;
      renderTicket();
    });
  }
  for (const id of ["tr-price", "tr-amount", "tr-total"]) {
    el(id).addEventListener("blur", () => settle(id));
    // While it is typed, a number is plain digits. Its groups come back
    // when the field is left.
    el<HTMLInputElement>(id).addEventListener("focus", (e) => {
      const input = e.target as HTMLInputElement;
      const digits = input.value.replace(/[\s\u00a0\u202f]/g, "");
      if (digits !== input.value) input.value = digits;
    });
  }
  el("tr-total").addEventListener("input", totalTyped);
  for (const button of document.querySelectorAll<HTMLButtonElement>("#tr-tif [data-tif]")) {
    button.addEventListener("click", () => {
      stay = button.dataset.tif as typeof stay;
      renderTicket();
    });
  }
  // The lists move when the page gains or loses its third column.
  new ResizeObserver(() => {
    const wide = el("tr-page").clientWidth;
    // Hidden with the page: nothing to learn from a width of nothing.
    if (wide === 0) return;
    const now = wide >= 1180 ? 3 : wide >= 740 ? 2 : 1;
    if (now === columns) return;
    columns = now;
    if (!info) return;
    placeLists();
    renderBook();
  }).observe(el("tr-page"));
  // A taller window gives the book more prices.
  window.addEventListener("resize", () => {
    if (info && mode === "pro" && bookRows() !== bookDepth) renderBook();
  });
  el("tr-pct").addEventListener("input", (e) => setPart(Number((e.target as HTMLInputElement).value)));
  for (const mark of document.querySelectorAll<HTMLElement>("#tr-slider [data-pct]")) {
    mark.addEventListener("click", () => {
      if (!el<HTMLInputElement>("tr-pct").disabled) setPart(Number(mark.dataset.pct));
    });
  }
  el("tr-form").addEventListener("submit", (e) => {
    e.preventDefault();
    void reviewOrder();
  });

  // The account and its dialogs.
  el("tr-funds-open").addEventListener("click", openFunds);
  el("tr-funds-copy").addEventListener("click", () => {
    if (info?.account) copyTo(info.account, el("tr-funds-copy"));
  });
  el("tr-send-open").addEventListener("click", openSend);
  for (const id of ["tr-funds-close", "tr-send-close", "tr-send-cancel"]) el(id).addEventListener("click", closeDialog);
  for (const id of ["tr-funds-backdrop", "tr-send-backdrop"]) {
    el(id).addEventListener("mousedown", (e) => {
      if (e.target === e.currentTarget) closeDialog();
    });
  }
  el("tr-send-form").addEventListener("input", renderSend);
  el("tr-send-asset").addEventListener("change", renderSend);
  el("tr-send-max").addEventListener("click", () => {
    const asset = sendAsset();
    if (!asset) return;
    el<HTMLInputElement>("tr-send-amount").value = plain(available(asset) ?? 0n, asset);
    renderSend();
  });
  el("tr-send-form").addEventListener("submit", (e) => {
    e.preventDefault();
    void reviewSend();
  });

  // Validators are asked for their status now and then while the page is
  // open, and a trade that takes long says so.
  setInterval(() => {
    if (!active || !info) return;
    if (Date.now() - healthRead > HEALTH_MS) void refreshHealth();
    for (const flow of flows.values()) {
      if (flow.stage === "sent" && !flow.slow && Date.now() - flow.at > SLOW_MS) {
        flow.slow = true;
        renderFlow(flow.origin);
      }
    }
  }, 1000);
}
