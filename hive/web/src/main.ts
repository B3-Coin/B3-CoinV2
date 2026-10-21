import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import { amountNode, amountText, amountUnits, bannerTone, el, formatAmount, icon, initTheme, isMasked, setMasked, shorten, sumAmounts, toggleTheme, type Theme } from "../shared/ui";
import { startTrade, stopTrade, tradeMaskChanged, tradeWalletChanged, tradeDecided, type TradeResult } from "./trade";
import { pickAddress, sortOwn } from "./picker";
import { single } from "./single";
import { tabKeys, trapDialog } from "./a11y";
import { confirmAction } from "./confirm";

// Mirrors NodeStatus in src-tauri/src/node.rs (serde tag "phase").
type NodeStatus =
  | { phase: "starting"; message: string }
  | { phase: "ready"; chain: string; blocks: number; best_block_hash: string }
  | { phase: "stopping" }
  | { phase: "failed"; error: string }
  | { phase: "setup" };

// Every tab list on the page moves with the arrow keys.
tabKeys();

// Mirrors WalletOverview in src-tauri/src/lib.rs and wallet::Staking.
type Staking = {
  tip_height: number;
  next_block_phase: string;
  modern_pos_active: boolean;
  running: boolean;
  this_wallet: boolean;
  state: string;
  last_error: string | null;
  finality_signing: boolean;
  last_signed_height: number;
};
type WalletOverview = {
  wallet: string;
  spendable: string;
  pending: string;
  immature: string;
  security: "watch_only" | "unencrypted" | "locked" | "unlocked";
  staking: Staking | null;
  staking_error: string | null;
  can_fund: boolean;
};

// Mirrors ChainOverview and Peer in src-tauri/src/chain.rs.
type ChainOverview = {
  chain: string;
  blocks: number;
  headers: number;
  best_block_hash: string;
  verification_progress: number;
  initial_block_download: boolean;
  median_time: number;
  size_on_disk: number;
  pruned: boolean;
  user_agent: string;
  protocol_version: number;
  connections_in: number;
  connections_out: number;
  network_active: boolean;
  mempool_transactions: number;
  mempool_bytes: number;
  warnings: string[];
};

type Peer = {
  id: number;
  address: string;
  inbound: boolean;
  connection_type: string;
  user_agent: string;
  version: number;
  synced_blocks: number;
  ping_ms: number | null;
  bytes_sent: number;
  bytes_received: number;
  connected_since: number;
  network: string;
  services: string[];
  permissions: string[];
  transport: string;
  session_id: string;
  relays_transactions: boolean;
  high_bandwidth_to: boolean;
  high_bandwidth_from: boolean;
  synced_headers: number;
  last_send: number;
  last_receive: number;
  last_block: number;
  last_transaction: number;
  min_ping_ms: number | null;
  ping_wait_ms: number | null;
  time_offset: number;
  mapped_as: number | null;
};

// Mirrors NodeInfo in src-tauri/src/lib.rs (chain::NodeDetails, flattened).
type NodeInfo = {
  uptime: number;
  local_addresses: string[];
  mempool_usage: number;
  mempool_max: number;
  total_received: number;
  total_sent: number;
  banned: { address: string; created: number; until: number }[];
  datadir: string;
  debug_log: string;
};

// Mirrors ActivityEntry in src-tauri/src/wallet.rs.
type ActivityEntry = {
  txid: string;
  category: string;
  amount: string;
  fee: string | null;
  confirmations: number;
  time: number;
  address: string;
  label: string;
};

// Mirrors DraftResult in src-tauri/src/lib.rs (serde tag "outcome").
type DraftResult =
  | { outcome: "sent"; id: string; txid: string }
  | { outcome: "signed"; id: string; psbt: string; complete: boolean }
  | { outcome: "rejected"; id: string }
  | { outcome: "failed"; id: string; error: string }
  // A FlowMesh order or cancellation; the Trade page handles its own.
  | { outcome: "traded"; id: string };

function showThemeIcon(theme: Theme): void {
  // The button shows, and is named by, the theme it switches to.
  const next = theme === "dark" ? "light" : "dark";
  el("theme-icon").setAttribute("href", `/icons.svg#i-${theme === "dark" ? "sun" : "moon"}`);
  el("theme").setAttribute("aria-label", `Switch to the ${next} theme`);
  el("theme").title = `Switch to the ${next} theme`;
}
showThemeIcon(await initTheme());

// ---- views -----------------------------------------------------------------

const VIEWS = {
  setup: { title: "Set up B3 Hive", sub: "Get the B3 chain for your node" },
  home: { title: "Home", sub: "Your balance and your node" },
  send: { title: "Send", sub: "Pay someone in B3" },
  receive: { title: "Receive", sub: "Addresses and payment links for this wallet" },
  activity: { title: "Activity", sub: "Your wallet's transactions" },
  book: { title: "Address book", sub: "Addresses you pay, with their labels" },
  stake: { title: "Stake", sub: "Secure B3 with this wallet's validator" },
  trade: { title: "Trade", sub: "FlowMesh token markets, settled in B3" },
  wallets: { title: "Wallets", sub: "Open, create, switch and back up wallets" },
  network: { title: "Network", sub: "Your node and its peers" },
  settings: { title: "Settings", sub: "How your node runs and connects" },
  updates: { title: "Updates", sub: "Signed B3 Hive releases" },
} as const;
type View = keyof typeof VIEWS;

let current: View = "home";

function go(view: View): void {
  current = view;
  for (const id of Object.keys(VIEWS)) el(`view-${id}`).hidden = id !== view;
  for (const item of document.querySelectorAll<HTMLElement>(".nav-item[data-view]")) {
    if (item.dataset.view === view) item.setAttribute("aria-current", "page");
    else item.removeAttribute("aria-current");
  }
  el("page-title").textContent = VIEWS[view].title;
  el("page-sub").textContent = VIEWS[view].sub;
  if (view === "send") document.querySelector<HTMLInputElement>("#recipients .r-address")?.focus();
  if (view === "updates") void refreshUpdates();
  if (view === "activity") void refreshActivity();
  if (view === "network") {
    void refreshNetwork();
    drawTraffic();
  }
  if (view === "settings") void loadSettings();
  if (view === "wallets") void refreshWallets();
  if (view === "receive") void refreshReceiving();
  if (view === "book") void refreshBook();
  if (view === "stake") void refreshStake();
  if (view === "send") void refreshBook();
  if (view === "trade") startTrade();
  else stopTrade();
}

for (const button of document.querySelectorAll<HTMLElement>("[data-view]")) {
  button.addEventListener("click", () => go(button.dataset.view as View));
}

el("open-console").addEventListener("click", () => {
  invoke("open_console").catch((e) => showBanner("danger", "The console did not open", String(e)));
});

el("restart-node").addEventListener("click", async () => {
  const button = el<HTMLButtonElement>("restart-node");
  button.disabled = true;
  try {
    // The new node's status arrives as node-status events.
    wasReady = false;
    await invoke("restart_node");
  } catch (e) {
    el("node-banner-detail").textContent = String(e);
  } finally {
    button.disabled = false;
  }
});

el("theme").addEventListener("click", async () => showThemeIcon(await toggleTheme()));

// Qt's "Mask values": amounts on these pages show as dots until turned off.
// Remembered on this computer only; exports and the approval window are
// never masked.
const MASK_KEY = "hive.mask-values";

function applyMask(on: boolean): void {
  setMasked(on);
  const button = el("mask");
  button.setAttribute("aria-pressed", String(on));
  const label = on ? "Show amounts" : "Hide amounts";
  button.setAttribute("aria-label", label);
  button.title = label;
  el("mask-icon").setAttribute("href", `/icons.svg#i-${on ? "eye-off" : "eye"}`);
  tradeMaskChanged();
}

function storedMask(): boolean {
  try {
    return localStorage.getItem(MASK_KEY) === "1";
  } catch {
    return false;
  }
}

applyMask(storedMask());
el("mask").addEventListener("click", () => {
  const on = el("mask").getAttribute("aria-pressed") !== "true";
  try {
    localStorage.setItem(MASK_KEY, on ? "1" : "0");
  } catch {
    // Not remembered; the choice still applies until B3 Hive closes.
  }
  applyMask(on);
  if (!el("detail-backdrop").hidden) closeDetails();
  if (wasReady) {
    void refreshWallet();
    void refreshRecent();
    if (current === "activity") void refreshActivity();
    if (current === "stake") void refreshStake();
  }
});

// ---- node status -----------------------------------------------------------

type Tone = "info" | "danger";

function showBanner(tone: Tone, title: string, detail: string): void {
  const banner = el("node-banner");
  banner.hidden = false;
  bannerTone(banner, tone);
  el("node-banner-spinner").hidden = tone !== "info";
  el("node-banner-icon").hidden = tone === "info";
  el("node-banner-title").textContent = title;
  el("node-banner-detail").textContent = detail;
  el("node-banner-progress").hidden = true;
  el("node-banner-elapsed").hidden = true;
  el("restart-node").hidden = true;
}

function badge(state: "unlocked" | "locked" | "no-node" | "no-wallet", text: string): void {
  el("node-badge").dataset.state = state;
  el("node-badge-text").textContent = text;
  el("node-badge").title = text;
}

// How long the node has been starting, so a long step does not look stuck.
let startingSince: number | null = null;

function showStartingTime(): void {
  const box = el("node-banner-elapsed");
  if (startingSince === null || el("node-banner").hidden) return;
  const seconds = Math.round((Date.now() - startingSince) / 1000);
  box.hidden = seconds < 5;
  box.textContent =
    `Starting for ${seconds < 60 ? `${seconds} s` : `${Math.floor(seconds / 60)} min ${seconds % 60} s`}.` +
    (seconds >= 30 ? " Loading a large chain can take several minutes." : "");
}

window.setInterval(() => {
  if (startingSince !== null) showStartingTime();
}, 1000);

// A failure after the node was running is a stop or a crash, not a start error.
let wasReady = false;

function render(status: NodeStatus): void {
  const ready = status.phase === "ready";
  wasReady ||= ready;
  el("chain").hidden = !ready;
  // The wizard is the only page until the node has a chain.
  const setup = status.phase === "setup";
  el("rail-nav").hidden = setup;
  if (setup) {
    el("node-banner").hidden = true;
    badge("no-node", "Not set up");
    go("setup");
    void loadSetup();
    return;
  }
  if (current === "setup") go("home");
  switch (status.phase) {
    case "starting": {
      // Long steps (loading the block index, verifying blocks, rescans)
      // end their message with a percentage.
      const step = /^(.*?)\s*(\d{1,3})%$/.exec(status.message);
      showBanner("info", "Starting the node", step ? `${step[1]} ${step[2]}%` : status.message);
      el("node-banner-progress").hidden = !step;
      if (step) el("node-banner-bar").style.width = `${Math.min(100, Number(step[2]))}%`;
      startingSince ??= Date.now();
      showStartingTime();
      badge("no-wallet", "Starting");
      break;
    }
    case "ready":
      startingSince = null;
      el("node-banner").hidden = true;
      badge("unlocked", `Running on ${status.chain}`);
      el("chain-name").textContent = status.chain;
      el("chain-blocks").textContent = status.blocks.toLocaleString("en-US");
      el("chain-best").textContent = shorten(status.best_block_hash, 10, 8);
      el("chain-best").title = status.best_block_hash;
      void refreshWallet();
      void refreshOverview();
      void refreshRecent();
      void refreshWallets();
      if (current === "activity") void refreshActivity();
      break;
    case "stopping":
      startingSince = null;
      showBanner("info", "Stopping the node", "");
      badge("locked", "Stopping");
      break;
    case "failed":
      startingSince = null;
      showBanner("danger", wasReady ? "The node stopped" : "The node could not start", status.error);
      el("restart-node").hidden = false;
      badge("no-node", "Not running");
      el("wallet").hidden = true;
      el("no-wallet").hidden = true;
      el("recent").hidden = true;
      el("node-warnings").hidden = true;
      break;
  }
}

// ---- wallet ----------------------------------------------------------------

const SECURITY: Record<WalletOverview["security"], [string, string, string]> = {
  watch_only: ["Watch-only", "neutral", "i-eye"],
  unencrypted: ["Not encrypted", "warning", "i-unlock"],
  locked: ["Locked", "success", "i-lock"],
  unlocked: ["Unlocked", "info", "i-unlock"],
};

// The era names and lines of Qt's dashboard.
function era(s: Staking): [string, string] {
  switch (s.next_block_phase) {
    case "modern":
      return ["Test network", "This chain does not use the live legacy B3 transition boundary."];
    case "legacy":
      return ["Legacy network", "Legacy operation continues through the sealed transition boundary."];
    case "corridor":
      return ["Transition corridor", "The temporary proof-of-work corridor is active on the connected chain."];
    case "modern_pos":
      return s.modern_pos_active
        ? ["Modern PoS", "Modern proof of stake is active on the connected chain."]
        : [
            "Modern PoS unavailable",
            "The modern proof-of-stake rules are not completely configured; the node fails closed.",
          ];
    default:
      return [s.next_block_phase || "Unknown", "The node reported an era B3 Hive does not know."];
  }
}

function renderStaking(s: Staking | null, error: string | null): void {
  const lines: string[] = [];
  const badgeEl = el("staking-badge");
  if (!s) {
    el("era-name").textContent = "Unavailable";
    el("era-detail").textContent = "The node did not report its staking status.";
    el("era-height").textContent = "";
    badgeEl.textContent = "Unavailable";
    badgeEl.className = "badge badge-warning";
    lines.push(`Staking status unavailable: ${error ?? "no answer"}`);
  } else {
    const [name, detail] = era(s);
    el("era-name").textContent = name;
    el("era-detail").textContent = detail;
    el("era-height").textContent = s.tip_height >= 0 ? `Block ${count(s.tip_height)}` : "";
    const mine = s.running && s.this_wallet;
    badgeEl.textContent = mine ? "On" : s.running ? "Other wallet" : "Off";
    badgeEl.className = `badge badge-${mine ? "success" : "neutral"}`;
    if (mine) {
      lines.push("Staking is on.");
      if (s.state) lines.push(s.state);
    } else if (s.running) {
      lines.push("Another wallet is staking on this node.");
    } else {
      lines.push("Staking is off.");
    }
    if (s.finality_signing && s.this_wallet) {
      lines.push(
        s.last_signed_height >= 0
          ? `Finality signing enabled · last signed ${count(s.last_signed_height)}`
          : "Finality signing enabled · no checkpoint signed yet",
      );
    } else if (s.finality_signing) {
      lines.push("Finality signing belongs to another wallet.");
    } else {
      lines.push("Finality signing is not armed.");
    }
    if (s.last_error) lines.push(`Last reported error: ${s.last_error}`);
  }
  el("staking-lines").replaceChildren(
    ...lines.map((line) => {
      const li = document.createElement("li");
      li.textContent = line;
      return li;
    }),
  );
}

// The wallet card's last good read, so a failed one can say how old it is.
let walletReadAt: number | null = null;
let walletReads = 0;

/** A failed read leaves the last balance up, marked with its age, never
 *  passed off as current. */
function walletStale(error: string | null): void {
  el("wallet").classList.toggle("is-stale", error !== null);
  const note = el("wallet-stale");
  note.hidden = error === null;
  if (error === null) return;
  note.textContent =
    walletReadAt === null
      ? "This wallet could not be read. Trying again…"
      : `Not updated since ${readTime(walletReadAt)}: the wallet could not be read. Trying again…`;
  note.title = error;
}

/** Another wallet is in use: nothing of the previous one stays up. */
function clearWalletCard(): void {
  walletReads++;
  walletReadAt = null;
  el("wallet-name").textContent = "Loading…";
  el("spendable").replaceChildren(amountNode("—", "amt-hero"));
  el("pending-chip").hidden = el("immature-chip").hidden = true;
  el("staking-lines").replaceChildren();
  walletStale(null);
}

// Whether the node was staking at the last read: a restart stops it.
let stakingRunning = false;

async function refreshWallet(): Promise<void> {
  const mine = ++walletReads;
  try {
    const w = await invoke<WalletOverview | null>("wallet_overview");
    if (mine !== walletReads) return;
    walletReadAt = Date.now();
    walletStale(null);
    el("no-wallet").hidden = w !== null;
    el("wallet").hidden = w === null;
    el("era-card").hidden = el("staking-card").hidden = w === null;
    if (!w) return;
    el("wallet-name").textContent = `Wallet ${w.wallet}`;
    el("spendable").replaceChildren(amountNode(w.spendable, "amt-hero"));
    const zero = (a: string) => /^0*\.?0*$/.test(a);
    el("pending-chip").hidden = zero(w.pending);
    el("pending").textContent = `${formatAmount(w.pending)} pending`;
    el("immature-chip").hidden = zero(w.immature);
    el("immature").textContent = `${formatAmount(w.immature)} maturing`;
    const [text, tone, icon] = SECURITY[w.security];
    el("security").textContent = text;
    el("security-chip").className = `badge badge-${tone}`;
    el("security-icon").setAttribute("href", `/icons.svg#${icon}`);
    renderStaking(w.staking, w.staking_error);
    stakingRunning = w.staking?.running ?? false;
    el("fund").hidden = !w.can_fund;
  } catch (e) {
    if (mine !== walletReads) return;
    if (el("wallet").hidden && el("no-wallet").hidden) el("wallet").hidden = false;
    walletStale(String(e));
  }
}

el("fund").addEventListener("click", async () => {
  const button = el<HTMLButtonElement>("fund");
  button.disabled = true;
  try {
    await invoke("dev_fund");
    await refreshWallet();
  } catch (e) {
    showBanner("danger", "Mining regtest blocks failed", String(e));
  } finally {
    button.disabled = false;
  }
});

// ---- receive -----------------------------------------------------------------

// Mirror src-tauri/src/receive.rs.
type Qr = { width: number; modules: string };
type ReceiveRequest = {
  address: string;
  label: string;
  amount: string | null;
  message: string;
  uri: string;
  qr: Qr | null;
};
// Mirrors receive::Receiving: one of the wallet's own addresses.
type Receiving = {
  address: string;
  label: string;
  received: string;
  transactions: number;
  balance: string;
  coins: number;
  change: boolean;
};

function drawQr(svg: Element, qr: Qr): void {
  // A 4-module quiet zone, as the QR standard asks.
  const size = qr.width + 8;
  svg.setAttribute("viewBox", `0 0 ${size} ${size}`);
  svg.setAttribute("shape-rendering", "crispEdges");
  const rects: SVGElement[] = [];
  for (let y = 0; y < qr.width; y++) {
    for (let x = 0; x < qr.width; x++) {
      if (qr.modules[y * qr.width + x] === "1") {
        rects.push(svgNode("rect", { x: String(x + 4), y: String(y + 4), width: "1", height: "1" }));
      }
    }
  }
  svg.replaceChildren(...rects);
}

function copyButton(button: HTMLButtonElement, text: () => string, done = "Copied"): void {
  const label = button.querySelector("span")?.textContent ?? "";
  button.addEventListener("click", (e) => {
    e.stopPropagation();
    const value = text();
    if (!value) return;
    void navigator.clipboard.writeText(value).then(() => {
      const span = button.querySelector("span");
      if (span) {
        span.textContent = done;
        setTimeout(() => (span.textContent = label), 1500);
      } else {
        button.title = done;
      }
    });
  });
}

let ownAddresses: Receiving[] = [];
// The address the Receive page shows; kept across refreshes while it exists.
let receiveShown: string | null = null;
let receiveRequest: ReceiveRequest | null = null;
let receiveFilter: "all" | "coins" | "unused" = "all";

const isUnused = (a: Receiving) => !a.change && a.transactions === 0 && a.coins === 0;

// What Receive shows first: an unused address without a label, since a
// labelled one was likely made for someone; else any unused one; else the
// first receiving address.
/** Labels Hive gives addresses it makes for one purpose (flowmesh.rs
 *  WITHDRAWAL_LABEL): fine to receive on, but not offered by default. */
const PURPOSE_LABELS = new Set(["FlowMesh withdrawal"]);

function defaultReceiving(): Receiving | null {
  const receiving = ownAddresses.filter((a) => !a.change);
  const offered = receiving.filter((a) => !PURPOSE_LABELS.has(a.label));
  return (
    offered.find((a) => isUnused(a) && !a.label) ?? offered.find(isUnused) ?? offered[0] ?? receiving[0] ?? null
  );
}

function receiveResult(text: string): void {
  el("rcv-result").hidden = !text;
  el("rcv-result").textContent = text;
}

function renderRequest(): void {
  const r = receiveRequest;
  el("rcv-none").hidden = r !== null;
  el("rcv-show").hidden = r === null;
  if (!r) return;
  const own = ownAddresses.find((a) => a.address === r.address);
  el("rcv-qr").hidden = r.qr === null;
  el("rcv-qr-note").hidden = r.qr !== null;
  if (r.qr) drawQr(el("rcv-qr"), r.qr);
  el("rcv-label-text").textContent = r.label || "No label";
  el("rcv-label-text").classList.toggle("muted", !r.label);
  el("rcv-addr").textContent = r.address;
  el("rcv-change-note").hidden = !own?.change;
  const used = own && !isUnused(own);
  el("rcv-used-note").hidden = !used || own.change;
  el("rcv-new-hint").hidden = !!used;
  // The requested amount is shown exactly, never masked: it is what the
  // payer will be asked for.
  el("rcv-asked").hidden = r.amount === null && !r.message;
  el("rcv-asked").textContent = [r.amount === null ? "" : `Asks for ${r.amount} B3`, r.message ? `“${r.message}”` : ""]
    .filter(Boolean)
    .join(" · ");
}

async function showReceiving(address: string): Promise<void> {
  try {
    receiveRequest = await invoke<ReceiveRequest>("receive_show", {
      address,
      amount: el<HTMLInputElement>("rcv-amount").value,
      message: el<HTMLInputElement>("rcv-message").value,
    });
    receiveShown = address;
    receiveResult("");
  } catch (e) {
    receiveResult(String(e));
    return;
  }
  stopRename();
  renderRequest();
  renderOwnList();
}

function renderOwnList(): void {
  const q = el<HTMLInputElement>("rcv-search").value.trim().toLowerCase();
  const list = ownAddresses.filter(
    (a) =>
      (receiveFilter === "all" || (receiveFilter === "coins" ? a.coins > 0 : isUnused(a))) &&
      (!q || a.label.toLowerCase().includes(q) || a.address.toLowerCase().includes(q)),
  );
  el("rcv-count").textContent = ownAddresses.length ? plural(ownAddresses.length, "address", "addresses") : "";
  el("rcv-tools").hidden = ownAddresses.length < 2;
  el("rcv-empty").hidden = list.length > 0;
  el("rcv-empty").textContent = ownAddresses.length
    ? "No address matches."
    : "This wallet has no addresses yet.";
  el("rcv-list").replaceChildren(
    ...list.map((a) => {
      const row = document.createElement("div");
      row.className = "list-row clickable rcv-row";
      row.tabIndex = 0;
      row.setAttribute("role", "button");
      row.title = "Show this address";
      if (a.address === receiveShown) row.setAttribute("aria-current", "true");
      const lead = document.createElement("div");
      lead.className = a.change ? "lead" : "lead in";
      lead.append(icon(a.change ? "repeat" : "arrow-down"));
      const body = document.createElement("div");
      body.className = "body";
      const title = document.createElement("span");
      title.className = "row gap-2";
      const name = document.createElement("span");
      name.textContent = a.label || "No label";
      if (!a.label) name.className = "muted";
      title.append(name);
      if (a.change) {
        const badge = document.createElement("span");
        badge.className = "badge badge-neutral";
        // Change has no label; the node labels the addresses it keeps for
        // staking or FlowMesh.
        badge.textContent = a.label ? "Wallet's own" : "Change";
        badge.title = a.label
          ? "The node made this address for its own use (staking or FlowMesh). Give out a receiving address instead."
          : "The wallet made this address for change from your own payments.";
        title.append(badge);
      } else if (isUnused(a)) {
        const badge = document.createElement("span");
        badge.className = "badge badge-success";
        badge.textContent = "Unused";
        title.append(badge);
      }
      const meta = document.createElement("span");
      meta.className = "meta is-mono rcv-row-addr";
      meta.textContent = a.address;
      body.append(title, meta);
      const trail = document.createElement("div");
      trail.className = "trail";
      const what = document.createElement("span");
      what.className = "t-sm muted";
      what.textContent = a.coins
        ? plural(a.coins, "coin")
        : a.transactions
          ? `Received ${plural(a.transactions, "payment")}`
          : "Nothing received";
      trail.append(a.coins ? amountNode(a.balance) : document.createElement("span"), what);
      const copy = document.createElement("button");
      copy.type = "button";
      copy.className = "btn btn-ghost btn-icon btn-sm";
      copy.setAttribute("aria-label", "Copy address");
      copy.title = "Copy address";
      copy.append(icon("copy", "i-sm"));
      copyButton(copy, () => a.address);
      row.append(lead, body, trail, copy);
      const open = () => void showReceiving(a.address);
      row.addEventListener("click", open);
      row.addEventListener("keydown", (e) => {
        if (e.key === "Enter" || e.key === " ") {
          e.preventDefault();
          open();
        }
      });
      return row;
    }),
  );
}

const refreshReceiving = single(async (current) => {
  try {
    const list = await invoke<Receiving[]>("receive_addresses");
    if (!current()) return;
    ownAddresses = sortOwn(list);
  } catch (e) {
    if (!current()) return;
    receiveResult(String(e));
    return;
  }
  const keep = receiveShown && ownAddresses.some((a) => a.address === receiveShown) ? receiveShown : null;
  const target = keep ?? defaultReceiving()?.address ?? null;
  if (target) await showReceiving(target);
  else {
    receiveShown = null;
    receiveRequest = null;
    renderRequest();
    renderOwnList();
  }
});

function stopRename(): void {
  el("rcv-rename-form").hidden = true;
  el("rcv-label-row").hidden = false;
}

el("rcv-rename").addEventListener("click", () => {
  el<HTMLInputElement>("rcv-rename-input").value = receiveRequest?.label ?? "";
  el("rcv-label-row").hidden = true;
  el("rcv-rename-form").hidden = false;
  el<HTMLInputElement>("rcv-rename-input").focus();
});
el("rcv-rename-cancel").addEventListener("click", stopRename);
el("rcv-rename-form").addEventListener("keydown", (e) => {
  if ((e as KeyboardEvent).key === "Escape") stopRename();
});
el("rcv-rename-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  if (!receiveShown) return;
  try {
    await invoke("receive_rename", { address: receiveShown, label: el<HTMLInputElement>("rcv-rename-input").value });
  } catch (err) {
    receiveResult(String(err));
    return;
  }
  stopRename();
  await refreshReceiving();
});

copyButton(el<HTMLButtonElement>("rcv-copy"), () => receiveRequest?.address ?? "");
copyButton(el<HTMLButtonElement>("rcv-copy-link"), () => receiveRequest?.uri ?? "");

// The amount and message only change the link and QR code.
let askTimer: number | undefined;
for (const id of ["rcv-amount", "rcv-message"]) {
  el(id).addEventListener("input", () => {
    clearTimeout(askTimer);
    askTimer = window.setTimeout(() => {
      if (receiveShown) void showReceiving(receiveShown);
    }, 300);
  });
}

el("rcv-search").addEventListener("input", renderOwnList);
for (const button of document.querySelectorAll<HTMLButtonElement>("#rcv-filter [data-filter]")) {
  button.addEventListener("click", () => {
    receiveFilter = button.dataset.filter as typeof receiveFilter;
    for (const b of document.querySelectorAll<HTMLButtonElement>("#rcv-filter [data-filter]")) {
      b.setAttribute("aria-pressed", String(b === button));
    }
    renderOwnList();
  });
}

function openNewAddress(): void {
  el("rcv-new").hidden = false;
  el<HTMLInputElement>("rcv-label").focus();
}
el("rcv-new-open").addEventListener("click", openNewAddress);
el("rcv-first").addEventListener("click", openNewAddress);
el("rcv-new-cancel").addEventListener("click", () => {
  el<HTMLFormElement>("rcv-new").reset();
  el("rcv-new").hidden = true;
});

el("rcv-new").addEventListener("submit", async (e) => {
  e.preventDefault();
  const button = el<HTMLButtonElement>("rcv-submit");
  button.disabled = true;
  receiveResult("");
  try {
    const r = await invoke<ReceiveRequest>("receive_new", {
      label: el<HTMLInputElement>("rcv-label").value,
      amount: "",
      message: "",
    });
    el<HTMLFormElement>("rcv-new").reset();
    el("rcv-new").hidden = true;
    receiveShown = r.address;
    await refreshReceiving();
  } catch (err) {
    receiveResult(String(err));
  } finally {
    button.disabled = false;
  }
});

// ---- wallets -----------------------------------------------------------------
// Qt's wallet selector and File menu. The default wallet's name is "".

// Mirrors WalletsView in src-tauri/src/lib.rs.
type WalletsView = {
  loaded: string[];
  available: string[];
  current: string | null;
  security: WalletOverview["security"] | null;
};
type WalletAction = { action: "switch" | "open" | "close" | "create"; name: string };

const walletName = (name: string) => (name === "" ? "Default wallet" : name);

function walletsResult(tone: "success" | "danger" | "info" | "", text: string): void {
  const box = el("wl-result");
  box.hidden = !text;
  bannerTone(box, tone);
  box.textContent = text;
  if (text) box.scrollIntoView({ block: "nearest" });
}

function walletRow(name: string, trail: HTMLElement[], meta: string): HTMLElement {
  const row = document.createElement("div");
  row.className = "list-row";
  const lead = document.createElement("div");
  lead.className = "lead";
  lead.append(icon("wallet"));
  const body = document.createElement("div");
  body.className = "body";
  const title = document.createElement("span");
  title.textContent = walletName(name);
  const sub = document.createElement("span");
  sub.className = "meta";
  sub.textContent = meta;
  body.append(title, sub);
  const end = document.createElement("div");
  end.className = "trail trail-row";
  end.append(...trail);
  row.append(lead, body, end);
  return row;
}

function actionButton(text: string, cls: string, run: (button: HTMLButtonElement) => void): HTMLButtonElement {
  const b = document.createElement("button");
  b.type = "button";
  b.className = `btn btn-sm ${cls}`;
  b.textContent = text;
  b.addEventListener("click", () => run(b));
  return b;
}

function renderWallets(v: WalletsView): void {
  el("wl-loaded").replaceChildren(
    ...v.loaded.map((name) => {
      const inUse = name === v.current;
      const trail: HTMLElement[] = [];
      if (inUse) {
        const b = document.createElement("span");
        b.className = "badge badge-success";
        b.textContent = "In use";
        trail.push(b);
      } else {
        trail.push(actionButton("Use", "btn-secondary", () => void manageWallet({ action: "switch", name })));
      }
      // Asked first, as Qt asks before closing a wallet.
      trail.push(
        actionButton("Close", "btn-ghost", async () => {
          const yes = await confirmAction(
            `Close ${walletName(name)}?`,
            [
              "It stays in the wallets folder and no longer loads when the node starts. You can open it again from this page.",
              "If it is staking, staking stops with it.",
            ],
            "Close wallet",
            true,
          );
          if (yes) void manageWallet({ action: "close", name });
        }),
      );
      return walletRow(name, trail, inUse ? "Loaded · these pages show this wallet" : "Loaded");
    }),
  );
  el("wl-loaded-empty").hidden = v.loaded.length > 0;
  el("wl-available").replaceChildren(
    ...v.available.map((name) =>
      walletRow(name, [actionButton("Open", "btn-secondary", () => void manageWallet({ action: "open", name }))], "Not loaded"),
    ),
  );
  el("wl-available-empty").hidden = v.available.length > 0;
  renderWalletSecurity(v);
  el("wl-backup-name").textContent = v.current === null ? "the wallet in use" : walletName(v.current);
  el<HTMLButtonElement>("wl-backup").disabled = v.current === null;
  renderWalletPick(v);
}

// Qt's Settings menu: Encrypt wallet, Change passphrase, and Lock wallet.
const SECURITY_TEXT: Record<WalletOverview["security"], string> = {
  watch_only: "This wallet holds no private keys, so there is nothing to encrypt.",
  unencrypted: "Anyone who can read the wallet file can spend from it. Encrypting asks for a passphrase before every payment.",
  locked: "Payments need the passphrase: the approval window asks for it, unlocks only to sign, and locks again.",
  unlocked: "The wallet is unlocked: it can sign without asking for the passphrase until it locks again.",
};

function renderWalletSecurity(v: WalletsView): void {
  const s = v.security;
  el("wl-security-card").hidden = s === null;
  if (s === null) return;
  const [text, tone] = SECURITY[s];
  el("wl-security").textContent = text;
  el("wl-security").className = `badge badge-${tone}`;
  el("wl-security-text").textContent = SECURITY_TEXT[s];
  el("wl-encrypt").hidden = s !== "unencrypted";
  el("wl-change").hidden = s !== "locked" && s !== "unlocked";
  el("wl-lock").hidden = s !== "unlocked";
  el("wl-backup-plain").hidden = s !== "unencrypted";
}

async function askPassphrase(kind: "encrypt" | "change", keepResult = false): Promise<void> {
  if (!keepResult) walletsResult("", "");
  try {
    // The outcome arrives as a passphrase-result event.
    await invoke("passphrase_request", { kind });
  } catch (e) {
    walletsResult("danger", String(e));
  }
}

el("wl-encrypt").addEventListener("click", () => void askPassphrase("encrypt"));
el("wl-change").addEventListener("click", () => void askPassphrase("change"));
el("wl-lock").addEventListener("click", async () => {
  try {
    await invoke("wallet_lock");
    walletsResult("success", "The wallet is locked.");
  } catch (e) {
    walletsResult("danger", String(e));
  }
  void refreshWallets();
  void refreshWallet();
});

// The rail switcher appears once two wallets are loaded, as Qt's selector does.
function renderWalletPick(v: WalletsView): void {
  el("wallet-pick-field").hidden = v.loaded.length < 2;
  const pick = el<HTMLSelectElement>("wallet-pick");
  pick.replaceChildren(
    ...v.loaded.map((name) => {
      const o = document.createElement("option");
      o.value = name;
      o.textContent = walletName(name);
      o.selected = name === v.current;
      return o;
    }),
  );
}

const refreshWallets = single(async () => {
  try {
    renderWallets(await invoke<WalletsView>("wallets"));
  } catch (e) {
    walletsResult("danger", String(e));
  }
});

let walletBusy = false;

/** True when the action went through. */
async function manageWallet(action: WalletAction): Promise<boolean> {
  if (walletBusy) return false;
  walletBusy = true;
  walletsResult("", "");
  try {
    const v = await invoke<WalletsView>("wallet_manage", { action });
    renderWallets(v);
    const done = {
      switch: "Now using",
      open: "Opened",
      close: "Closed",
      create: "Created",
    }[action.action];
    walletsResult("success", `${done} ${walletName(action.action === "create" ? (v.current ?? action.name) : action.name)}.`);
    // Every page shows the wallet in use, and nothing of the previous one.
    clearWalletCard();
    el("recent-list").replaceChildren();
    el("activity-list").replaceChildren();
    el("activity-count").textContent = "";
    activityResult("danger", "");
    shownCoins = [];
    chosenCoins.clear();
    if (el<HTMLInputElement>("send-cc").checked) renderCoins();
    contacts = [];
    void refreshWallet();
    void refreshRecent();
    void refreshBook();
    void refreshCoins();
    tradeWalletChanged();
    // Another wallet has other addresses.
    receiveShown = null;
    receiveRequest = null;
    ownAddresses = [];
    renderRequest();
    renderOwnList();
    if (current === "receive") void refreshReceiving();
    // The previous wallet's stakes and controls must not linger.
    clearStake();
    void refreshStake();
    return true;
  } catch (e) {
    walletsResult("danger", String(e));
    void refreshWallets();
    return false;
  } finally {
    walletBusy = false;
  }
}

el("wallet-pick").addEventListener("change", () => {
  void manageWallet({ action: "switch", name: el<HTMLSelectElement>("wallet-pick").value }).then(() => {
    if (current === "activity") void refreshActivity();
  });
});

el("wl-create").addEventListener("submit", async (e) => {
  e.preventDefault();
  const input = el<HTMLInputElement>("wl-name");
  const button = el<HTMLButtonElement>("wl-create-submit");
  button.disabled = true;
  const created = await manageWallet({ action: "create", name: input.value });
  button.disabled = false;
  if (!created) return;
  input.value = "";
  // The new wallet is the one in use, so the request is for it.
  if (el<HTMLInputElement>("wl-create-encrypt").checked) await askPassphrase("encrypt", true);
});

// Restore and legacy recovery: the core opens the file picker and returns
// null when it is cancelled. Both rescan, so they can take a long time.
type ImportView = {
  imported: { wallet: string; keys_imported: number; keys_already_present: number };
  wallets: WalletsView;
};

// The rescan banner: elapsed time always, a bar while the node reports one.
let scanStarted = 0;
let scanPercent: number | null = null;
let scanTimer: number | undefined;

function renderScan(): void {
  const seconds = Math.round((Date.now() - scanStarted) / 1000);
  const elapsed = seconds < 60 ? `${seconds} s` : `${Math.floor(seconds / 60)} min ${seconds % 60} s`;
  el("wl-scan-text").textContent =
    scanPercent === null ? `Running for ${elapsed}. Keep Hive open until it finishes.` : `${scanPercent}% after ${elapsed}. Keep Hive open until it finishes.`;
  el("wl-scan-progress").hidden = scanPercent === null;
  el("wl-scan-bar").style.width = `${scanPercent ?? 0}%`;
}

function endScan(): void {
  window.clearInterval(scanTimer);
  scanTimer = undefined;
  el("wl-scan").hidden = true;
}

await listen<{ wallet: string; percent: number | null }>("wallet-scan", (event) => {
  if (scanTimer === undefined) {
    scanStarted = Date.now();
    scanPercent = null;
    scanTimer = window.setInterval(renderScan, 1000);
    walletsResult("", "");
    el("wl-scan").hidden = false;
    el("wl-scan").scrollIntoView({ block: "nearest" });
  }
  if (event.payload.percent !== null) scanPercent = event.payload.percent;
  renderScan();
});

async function restoreWallet(legacy: boolean): Promise<void> {
  if (walletBusy) return;
  const name = el<HTMLInputElement>("wl-restore-name").value;
  const buttons = [el<HTMLButtonElement>("wl-restore-backup"), el<HTMLButtonElement>("wl-import-legacy")];
  walletBusy = true;
  buttons.forEach((b) => (b.disabled = true));
  walletsResult(
    "info",
    legacy
      ? "Choose the dump file. After that the node checks it and rescans the whole chain; keep Hive open until it finishes."
      : "Choose the backup file. After that the node loads it and rescans; keep Hive open until it finishes.",
  );
  try {
    let done: string | null = null;
    if (legacy) {
      const r = await invoke<ImportView | null>("wallet_import_legacy", { name });
      if (r) {
        renderWallets(r.wallets);
        const i = r.imported;
        done =
          `Recovered ${plural(i.keys_imported, "key")} into ${walletName(i.wallet)}` +
          (i.keys_already_present ? ` (${plural(i.keys_already_present, "key")} already there)` : "") +
          ", and it is now in use. Encrypt it and back it up, then delete the dump file.";
      }
    } else {
      const v = await invoke<WalletsView | null>("wallet_restore", { name });
      if (v) {
        renderWallets(v);
        done = `Restored ${walletName(v.current ?? name)}, and it is now in use.`;
      }
    }
    if (done === null) {
      walletsResult("", "");
      return;
    }
    walletsResult("success", done);
    el<HTMLInputElement>("wl-restore-name").value = "";
    void refreshWallet();
    void refreshRecent();
  } catch (e) {
    walletsResult("danger", String(e));
    void refreshWallets();
  } finally {
    endScan();
    walletBusy = false;
    buttons.forEach((b) => (b.disabled = false));
  }
}

el("wl-restore").addEventListener("submit", (e) => e.preventDefault());
el("wl-restore-backup").addEventListener("click", () => void restoreWallet(false));
el("wl-import-legacy").addEventListener("click", () => void restoreWallet(true));

el("wl-backup").addEventListener("click", async () => {
  const button = el<HTMLButtonElement>("wl-backup");
  button.disabled = true;
  try {
    const path = await invoke<string | null>("wallet_backup");
    if (path !== null) walletsResult("success", `Saved the backup to ${path}`);
  } catch (e) {
    walletsResult("danger", String(e));
  } finally {
    button.disabled = false;
  }
});

// ---- formatting ------------------------------------------------------------

function count(n: number): string {
  return n.toLocaleString("en-US");
}

function plural(n: number, one: string, many = `${one}s`): string {
  return `${count(n)} ${n === 1 ? one : many}`;
}

function bytes(n: number): string {
  const units = ["B", "kB", "MB", "GB", "TB"];
  let value = n;
  let unit = 0;
  while (value >= 1000 && unit < units.length - 1) {
    value /= 1000;
    unit++;
  }
  return `${value.toLocaleString("en-US", { maximumFractionDigits: unit === 0 ? 0 : 1 })} ${units[unit]}`;
}

function dateTime(unix: number): string {
  return new Date(unix * 1000).toLocaleString("en-US", { dateStyle: "medium", timeStyle: "short" });
}

/** "4 min ago" for recent times, otherwise the date. */
function ago(unix: number): string {
  const seconds = Math.floor(Date.now() / 1000) - unix;
  if (seconds < 60) return "just now";
  if (seconds < 3600) return `${Math.floor(seconds / 60)} min ago`;
  if (seconds < 86400) return `${Math.floor(seconds / 3600)} h ago`;
  return dateTime(unix);
}

/** Runs `refresh` unless the previous run is still going. */
/** The time of day a read was made, for "not updated since" notes. */
function readTime(at: number): string {
  return new Date(at).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" });
}

// ---- dashboard -------------------------------------------------------------

const refreshOverview = single(async () => {
  let o: ChainOverview;
  try {
    o = await invoke<ChainOverview>("chain_overview");
  } catch {
    // The node stopped or is busy; its status banner says which.
    return;
  }
  // The node counts itself behind until its tip is recent. Without peers
  // nothing is downloading, so say that rather than "Syncing".
  const peers = o.connections_in + o.connections_out;
  const syncing = o.initial_block_download || o.headers > o.blocks;
  const [stateText, tone] = !syncing
    ? ["Running", "success"]
    : peers === 0
      ? ["Behind, no peers", "warning"]
      : ["Syncing", "info"];
  const percent = (o.verification_progress * 100).toLocaleString("en-US", { maximumFractionDigits: 2 });
  el("sync-badge").className = `badge badge-${tone}`;
  el("sync-badge-text").textContent = stateText;
  el("chain-blocks").textContent = o.headers > o.blocks ? `${count(o.blocks)} of ${count(o.headers)}` : count(o.blocks);
  el("chain-time").textContent = o.blocks > 0 ? ago(o.median_time) : "—";
  el("chain-time").title = o.blocks > 0 ? dateTime(o.median_time) : "";
  el("chain-peers").textContent = o.network_active
    ? `${count(peers)} (${count(o.connections_out)} out, ${count(o.connections_in)} in)`
    : "Network off";
  el("chain-mempool").textContent = `${plural(o.mempool_transactions, "transaction")}, ${bytes(o.mempool_bytes)}`;
  el("sync").hidden = !syncing;
  el("sync-percent").textContent = `${percent} %`;
  el("sync-bar").style.width = `${o.verification_progress * 100}%`;
  el("node-warnings").hidden = o.warnings.length === 0;
  el("node-warnings-text").textContent = o.warnings.join(" ");
  renderNodeInfo(o);
});

// ---- activity --------------------------------------------------------------

const CATEGORY: Record<string, { text: string; lead: string; icon: string }> = {
  receive: { text: "Received", lead: "in", icon: "arrow-down" },
  send: { text: "Sent", lead: "out", icon: "arrow-up" },
  generate: { text: "Mined", lead: "stake", icon: "spark" },
  immature: { text: "Mined", lead: "stake", icon: "clock" },
  orphan: { text: "Mined, not accepted", lead: "", icon: "alert" },
};

/** As in Qt: six confirmations is the recommended number. A maturing
 *  reward is not spendable however many it has. */
function confirmationText(n: number, category: string): string {
  if (n < 0) return "Conflicts with the chain";
  if (n === 0) return "Waiting to confirm";
  if (category === "immature") return `Maturing, ${plural(n, "confirmation")}`;
  if (n < 6) return `${n} of 6 confirmations`;
  return "Confirmed";
}

function activityRow(e: ActivityEntry): HTMLElement {
  const kind = CATEGORY[e.category] ?? { text: e.category, lead: "", icon: "info" };
  const row = document.createElement("div");
  row.className = "list-row";
  const lead = document.createElement("div");
  lead.className = `lead ${kind.lead}`.trim();
  lead.append(icon(kind.icon));

  const body = document.createElement("div");
  body.className = "body";
  const title = document.createElement("strong");
  title.textContent = kind.text;
  const meta = document.createElement("span");
  meta.className = "meta mono";
  const who = e.label || shorten(e.address || e.txid, 10, 8);
  meta.textContent = `${who} · ${dateTime(e.time)}`;
  meta.title = e.address ? `Transaction ${e.txid}, address ${e.address}` : `Transaction ${e.txid}`;
  body.append(title, meta);

  const trail = document.createElement("div");
  trail.className = "trail";
  const status = document.createElement("span");
  status.className = "t-sm muted";
  status.textContent = confirmationText(e.confirmations, e.category);
  trail.append(amountNode(e.amount), status);
  if (e.fee) {
    const fee = document.createElement("span");
    fee.className = "t-sm muted";
    fee.append("Fee ", amountText(e.fee.replace(/^-/, "")));
    trail.append(fee);
  }

  row.append(lead, body, trail);
  return row;
}

function emptyState(title: string, text: string, iconName: string): HTMLElement {
  const box = document.createElement("div");
  box.className = "empty empty--tight";
  const badge = document.createElement("div");
  badge.className = "empty-icon";
  badge.append(icon(iconName));
  const h = document.createElement("h3");
  h.textContent = title;
  const p = document.createElement("p");
  p.textContent = text;
  box.append(badge, h, p);
  return box;
}

function activityList(lines: ActivityEntry[]): HTMLElement[] {
  return lines.length
    ? lines.map(activityRow)
    : [emptyState("No transactions yet", "Payments to and from this wallet appear here.", "activity")];
}

// Mirrors history::Filter, Page and Details in src-tauri.
type HistoryFilter = {
  kind: string;
  from: number | null;
  to: number | null;
  search: string;
  min_amount: string | null;
};
type HistoryPage = { entries: ActivityEntry[]; matching: number; total: number };
type DetailLine = { category: string; address: string; label: string; amount: string; vout: number };
type TxDetails = {
  txid: string;
  wtxid: string;
  amount: string;
  fee: string | null;
  confirmations: number;
  generated: boolean;
  abandoned: boolean;
  block_hash: string | null;
  block_height: number | null;
  time: number;
  time_received: number;
  comment: string;
  comment_to: string;
  size: number;
  lines: DetailLine[];
};

const ACTIVITY_STEP = 100;
let activityShown = ACTIVITY_STEP;

const refreshRecent = single(async (current) => {
  try {
    const lines = await invoke<ActivityEntry[]>("wallet_activity", { count: 5 });
    if (!current()) return;
    el("recent").hidden = false;
    el("recent-list").replaceChildren(...activityList(lines));
  } catch {
    if (!current()) return;
    // Never an empty list that reads as "no transactions".
    el("recent").hidden = false;
    el("recent-list").replaceChildren(
      emptyState("Not available", "Recent activity could not be read. It is read again with the next block.", "activity"),
    );
  }
});

function activityResult(tone: "danger" | "success", text: string): void {
  const box = el("activity-result");
  box.hidden = !text;
  bannerTone(box, tone, "mt-4");
  box.textContent = text;
}

/// Local midnight, as Unix seconds.
function midnight(d: Date): number {
  return Math.floor(new Date(d.getFullYear(), d.getMonth(), d.getDate()).getTime() / 1000);
}

/// Qt's date choices, in local time: [from, to), either end open.
function dateRange(choice: string): [number | null, number | null] {
  const now = new Date();
  const y = now.getFullYear();
  const m = now.getMonth();
  switch (choice) {
    case "today":
      return [midnight(now), null];
    case "week": {
      const monday = new Date(y, m, now.getDate() - ((now.getDay() + 6) % 7));
      return [midnight(monday), null];
    }
    case "month":
      return [midnight(new Date(y, m, 1)), null];
    case "lastmonth":
      return [midnight(new Date(y, m - 1, 1)), midnight(new Date(y, m, 1))];
    case "year":
      return [midnight(new Date(y, 0, 1)), null];
    case "range": {
      const from = el<HTMLInputElement>("act-from").valueAsDate;
      const to = el<HTMLInputElement>("act-to").valueAsDate;
      // valueAsDate is UTC midnight of the picked day; use that day locally.
      const local = (d: Date, extra: number) =>
        midnight(new Date(d.getUTCFullYear(), d.getUTCMonth(), d.getUTCDate() + extra));
      return [from ? local(from, 0) : null, to ? local(to, 1) : null];
    }
    default:
      return [null, null];
  }
}

function historyFilter(): HistoryFilter {
  const [from, to] = dateRange(el<HTMLSelectElement>("act-date").value);
  const min = el<HTMLInputElement>("act-min").value.trim();
  return {
    kind: el<HTMLSelectElement>("act-kind").value,
    from,
    to,
    search: el<HTMLInputElement>("act-search").value,
    min_amount: min || null,
  };
}

const refreshActivity = single(async (current) => {
  try {
    const page = await invoke<HistoryPage>("wallet_history", { filter: historyFilter(), count: activityShown });
    if (!current()) return;
    activityResult("danger", "");
    el("activity-list").replaceChildren(
      ...(page.entries.length
        ? page.entries.map((e) => clickableRow(e))
        : page.total
          ? [emptyState("Nothing matches", "No transaction passes these filters.", "search")]
          : [emptyState("No transactions yet", "Payments to and from this wallet appear here.", "activity")]),
    );
    el("activity-more-row").hidden = page.entries.length >= page.matching;
    el("activity-count").textContent = page.total
      ? page.matching === page.total
        ? plural(page.total, "line")
        : `${count(page.matching)} of ${plural(page.total, "line")}`
      : "";
  } catch (e) {
    if (!current()) return;
    activityResult("danger", `The wallet's transactions could not be read, so the list may be out of date: ${String(e)}`);
  }
});

function restartActivity(): void {
  activityShown = ACTIVITY_STEP;
  void refreshActivity();
}

let searchTimer: number | undefined;
el("act-search").addEventListener("input", () => {
  clearTimeout(searchTimer);
  searchTimer = window.setTimeout(restartActivity, 250);
});
el("act-min").addEventListener("change", restartActivity);
el("act-kind").addEventListener("change", restartActivity);
el("act-from").addEventListener("change", restartActivity);
el("act-to").addEventListener("change", restartActivity);
el("act-date").addEventListener("change", () => {
  const range = el<HTMLSelectElement>("act-date").value === "range";
  el("act-range").hidden = el("act-range-to").hidden = !range;
  restartActivity();
});

el("activity-more").addEventListener("click", () => {
  activityShown += ACTIVITY_STEP;
  void refreshActivity();
});

el("activity-export").addEventListener("click", async () => {
  const button = el<HTMLButtonElement>("activity-export");
  button.disabled = true;
  try {
    const path = await invoke<string | null>("wallet_export_csv", { filter: historyFilter() });
    if (path !== null) activityResult("success", `Saved the transactions shown by these filters to ${path}`);
  } catch (e) {
    activityResult("danger", `The transactions were not exported: ${String(e)}`);
  } finally {
    button.disabled = false;
  }
});

// ---- transaction details ---------------------------------------------------

function clickableRow(e: ActivityEntry): HTMLElement {
  const row = activityRow(e);
  row.classList.add("clickable");
  row.tabIndex = 0;
  row.setAttribute("role", "button");
  const open = () => void showTransaction(e.txid);
  row.addEventListener("click", open);
  row.addEventListener("keydown", (k) => {
    if (k.key === "Enter" || k.key === " ") {
      k.preventDefault();
      open();
    }
  });
  return row;
}

function detailRow(key: string, value: string | Node, copy?: string): HTMLElement {
  const row = document.createElement("div");
  row.className = "drow";
  const k = document.createElement("span");
  k.className = "dkey";
  k.textContent = key;
  const v = document.createElement("span");
  v.className = "dval";
  v.append(value);
  if (copy) {
    v.classList.add("is-mono", "wrap-any");
    const button = document.createElement("button");
    button.type = "button";
    button.className = "btn btn-ghost btn-icon btn-sm";
    button.setAttribute("aria-label", `Copy ${key.toLowerCase()}`);
    button.append(icon("copy"));
    button.addEventListener("click", () => {
      void navigator.clipboard.writeText(copy).then(() => (button.title = "Copied"));
    });
    v.append(" ", button);
  }
  row.append(k, v);
  return row;
}

let detailReturnFocus: HTMLElement | null = null;
let releaseDetails: (() => void) | null = null;

/// The details dialog shared by transactions and peers.
function openDetails(title: string, sub: string, rows: HTMLElement[]): void {
  detailReturnFocus = document.activeElement as HTMLElement | null;
  el("detail-title").textContent = title;
  el("detail-sub").textContent = sub;
  el("detail-rows").replaceChildren(...rows);
  el("detail-backdrop").hidden = false;
  releaseDetails ??= trapDialog(el("detail-backdrop"));
  el("detail-close").focus();
}

function closeDetails(): void {
  el("detail-backdrop").hidden = true;
  releaseDetails?.();
  releaseDetails = null;
  detailReturnFocus?.focus();
}

async function showTransaction(txid: string): Promise<void> {
  try {
    const d = await invoke<TxDetails>("wallet_transaction", { txid });
    const rows: HTMLElement[] = [];
    const status =
      d.confirmations < 0
        ? `Conflicted, ${count(-d.confirmations)} blocks deep`
        : d.abandoned
          ? "Abandoned"
          : // The node says per output whether mined coins have matured.
            confirmationText(
              d.confirmations,
              d.lines.some((l) => l.category === "immature") ? "immature" : "receive",
            );
    rows.push(detailRow("Status", d.confirmations === 0 && !d.abandoned ? "Unconfirmed" : status));
    rows.push(detailRow("Date", dateTime(d.time)));
    if (d.block_height !== null) rows.push(detailRow("Block", count(d.block_height)));
    // The node leaves maturing coins out of the net amount; the lines show them.
    if (!d.generated) rows.push(detailRow("Net amount", amountNode(d.amount)));
    if (d.fee) rows.push(detailRow("Fee", amountText(d.fee.replace(/^-/, ""))));
    for (const line of d.lines) {
      const kind = CATEGORY[line.category]?.text ?? line.category;
      const who = line.label ? `${line.label} (${line.address})` : line.address || "—";
      rows.push(detailRow(`${kind}, output ${line.vout}`, amountNode(line.amount)));
      if (line.address) rows.push(detailRow("Address", who, line.address));
    }
    if (d.comment) rows.push(detailRow("Comment", d.comment));
    if (d.comment_to) rows.push(detailRow("To", d.comment_to));
    if (d.generated) {
      rows.push(
        detailRow("Note", "Mined and staked coins can be spent once they have matured; until then they show as maturing."),
      );
    }
    rows.push(detailRow("Transaction id", d.txid, d.txid));
    if (d.wtxid && d.wtxid !== d.txid) rows.push(detailRow("Witness id", d.wtxid, d.wtxid));
    rows.push(detailRow("Size", bytes(d.size)));
    openDetails("Transaction", shorten(d.txid, 12, 12), rows);
  } catch (e) {
    activityResult("danger", `The transaction could not be read: ${String(e)}`);
  }
}

el("detail-close").addEventListener("click", closeDetails);
el("detail-backdrop").addEventListener("click", (e) => {
  if (e.target === el("detail-backdrop")) closeDetails();
});
document.addEventListener("keydown", (e) => {
  if (e.key === "Escape" && !el("detail-backdrop").hidden) closeDetails();
});

// ---- network ---------------------------------------------------------------

function renderNodeInfo(o: ChainOverview): void {
  const state = el("net-state");
  state.textContent = o.network_active ? "Network on" : "Network off";
  state.className = `badge badge-${o.network_active ? "success" : "warning"}`;
  el("net-agent").textContent = o.user_agent;
  el("net-protocol").textContent = String(o.protocol_version);
  el("net-connections").textContent = `${count(o.connections_out)} out, ${count(o.connections_in)} in`;
  el("net-height").textContent = `${count(o.blocks)} / ${count(o.headers)}`;
  el("net-disk").textContent = `${bytes(o.size_on_disk)}${o.pruned ? " (pruned)" : ""}`;
  el("peer-empty-text").textContent = !o.network_active
    ? "Networking is turned off on this node."
    : o.chain === "regtest"
      ? "This development node runs regtest with no peers."
      : "The node is not connected to anyone yet.";
}

const DIRECTION: Record<string, string> = {
  "outbound-full-relay": "Outbound",
  "block-relay-only": "Outbound, blocks only",
  manual: "Manual",
  feeler: "Feeler",
  "addr-fetch": "Address fetch",
  inbound: "Inbound",
};

function cell(text: string, className = "", title = ""): HTMLTableCellElement {
  const td = document.createElement("td");
  if (className) td.className = className;
  td.textContent = text;
  if (title) td.title = title;
  return td;
}

function when(unix: number): string {
  return unix > 0 ? `${ago(unix)} (${dateTime(unix)})` : "Never";
}

function ms(value: number | null): string {
  return value === null ? "—" : `${value < 10 ? value.toFixed(1) : Math.round(value)} ms`;
}

const yesNo = (b: boolean) => (b ? "Yes" : "No");

// Qt's peer details, in its order.
function showPeer(p: Peer): void {
  const direction = DIRECTION[p.connection_type] ?? (p.inbound ? "Inbound" : "Outbound");
  const hb = [p.high_bandwidth_to && "to", p.high_bandwidth_from && "from"].filter(Boolean).join(" and ");
  const rows = [
    detailRow("Permissions", p.permissions.join(", ") || "None"),
    detailRow("Direction / type", direction),
    detailRow("Transport", p.transport || "—"),
  ];
  if (p.session_id) rows.push(detailRow("Session id", p.session_id, p.session_id));
  rows.push(
    detailRow("Network", p.network || "—"),
    detailRow("Version", String(p.version || "—")),
    detailRow("User agent", p.user_agent || "—"),
    detailRow("Services", p.services.join(", ") || "None"),
    detailRow("Transaction relay", yesNo(p.relays_transactions)),
    detailRow("High bandwidth", hb || "No"),
    detailRow("Synced headers", p.synced_headers < 0 ? "—" : count(p.synced_headers)),
    detailRow("Synced blocks", p.synced_blocks < 0 ? "—" : count(p.synced_blocks)),
    detailRow("Connected", when(p.connected_since)),
    detailRow("Last block", when(p.last_block)),
    detailRow("Last transaction", when(p.last_transaction)),
    detailRow("Last send", when(p.last_send)),
    detailRow("Last receive", when(p.last_receive)),
    detailRow("Sent", bytes(p.bytes_sent)),
    detailRow("Received", bytes(p.bytes_received)),
    detailRow("Ping time", ms(p.ping_ms)),
    detailRow("Ping wait", ms(p.ping_wait_ms)),
    detailRow("Min ping", ms(p.min_ping_ms)),
    detailRow("Time offset", `${p.time_offset} s`),
    detailRow("Mapped AS", p.mapped_as === null ? "—" : String(p.mapped_as)),
  );
  openDetails(`Peer ${p.id}`, p.address, rows);
}

function peerRow(p: Peer): HTMLTableRowElement {
  const tr = document.createElement("tr");
  tr.className = "clickable";
  tr.title = "Show this peer's details";
  // The whole row takes a click; the address is the button the keyboard
  // and screen readers reach. Its click bubbles up to the row.
  tr.addEventListener("click", () => showPeer(p));
  const open = document.createElement("button");
  open.type = "button";
  open.className = "row-open";
  open.textContent = p.address;
  open.setAttribute("aria-label", `Details for peer ${p.address}`);
  const address = cell("", "mono");
  address.append(open);
  const direction = DIRECTION[p.connection_type] ?? (p.inbound ? "Inbound" : "Outbound");
  tr.append(
    address,
    cell(direction),
    cell(p.user_agent || "—", "mono", p.version ? `Protocol ${p.version}` : ""),
    cell(p.synced_blocks < 0 ? "—" : count(p.synced_blocks), "num"),
    cell(p.ping_ms === null ? "—" : `${Math.round(p.ping_ms)} ms`, "num"),
    cell(`${bytes(p.bytes_sent)} / ${bytes(p.bytes_received)}`, "num"),
    cell(ago(p.connected_since), "num", dateTime(p.connected_since)),
  );
  return tr;
}

function networkResult(text: string): void {
  const box = el("network-result");
  box.hidden = !text;
  bannerTone(box, "danger");
  box.textContent = text;
}

function renderNodeDetails(info: NodeInfo, mempoolTransactions: number): void {
  const started = Math.floor(Date.now() / 1000) - info.uptime;
  el("net-started").textContent = when(started);
  el("net-local").textContent = info.local_addresses.join(", ") || "None announced";
  el("net-traffic").textContent = `${bytes(info.total_received)} received, ${bytes(info.total_sent)} sent`;
  el("net-mempool").textContent =
    `${plural(mempoolTransactions, "transaction")}, ${bytes(info.mempool_usage)} of ${bytes(info.mempool_max)} memory`;
  el("net-datadir").textContent = info.datadir;
  el("net-debuglog").textContent = info.debug_log;
  el("ban-count").textContent = info.banned.length ? plural(info.banned.length, "address", "addresses") : "";
  el("ban-table").hidden = info.banned.length === 0;
  el("ban-empty").hidden = info.banned.length > 0;
  el("ban-rows").replaceChildren(
    ...info.banned.map((b) => {
      const tr = document.createElement("tr");
      tr.append(
        cell(b.address, "mono"),
        cell(ago(b.created), "num", dateTime(b.created)),
        cell(dateTime(b.until), "num"),
      );
      return tr;
    }),
  );
}

const refreshNetwork = single(async () => {
  try {
    const [overview, peers, info] = await Promise.all([
      invoke<ChainOverview>("chain_overview"),
      invoke<Peer[]>("network_peers"),
      invoke<NodeInfo>("node_info"),
    ]);
    networkResult("");
    renderNodeInfo(overview);
    renderNodeDetails(info, overview.mempool_transactions);
    el("peer-count").textContent = peers.length ? plural(peers.length, "peer") : "";
    el("peer-table").hidden = peers.length === 0;
    el("peer-empty").hidden = peers.length > 0;
    el("peer-rows").replaceChildren(...peers.map(peerRow));
  } catch (e) {
    networkResult(`The node's network state could not be read: ${String(e)}`);
  }
});

// ---- traffic graph ---------------------------------------------------------
// Sampled every REFRESH_MS while the node runs, so the graph has history
// when the page opens, as Qt's does. Rates are per second between samples.

type Totals = { received: number; sent: number; time_ms: number };
type Rate = { in: number; out: number };

const TRAFFIC_SAMPLES = 120; // 10 minutes at 5 seconds
const rates: Rate[] = [];
let lastTotals: Totals | null = null;

const sampleTraffic = single(async () => {
  try {
    const t = await invoke<Totals>("network_totals");
    const prev = lastTotals;
    lastTotals = t;
    // A restarted node starts its counters again.
    if (!prev || t.time_ms <= prev.time_ms || t.received < prev.received || t.sent < prev.sent) return;
    const seconds = (t.time_ms - prev.time_ms) / 1000;
    rates.push({ in: (t.received - prev.received) / seconds, out: (t.sent - prev.sent) / seconds });
    if (rates.length > TRAFFIC_SAMPLES) rates.shift();
    if (current === "network") drawTraffic();
  } catch {
    lastTotals = null;
  }
});

function svgNode(tag: string, attrs: Record<string, string>): SVGElement {
  const node = document.createElementNS("http://www.w3.org/2000/svg", tag);
  for (const [k, v] of Object.entries(attrs)) node.setAttribute(k, v);
  return node;
}

function drawTraffic(): void {
  const graph = el("traffic-graph");
  const [width, height] = [600, 160];
  const peak = Math.max(1, ...rates.map((r) => Math.max(r.in, r.out)));
  const grid = [0.25, 0.5, 0.75].map((f) =>
    svgNode("line", { class: "grid", x1: "0", x2: String(width), y1: String(height * f), y2: String(height * f) }),
  );
  const line = (pick: (r: Rate) => number, cls: string) => {
    // Newest sample at the right edge.
    const step = width / (TRAFFIC_SAMPLES - 1);
    const start = width - (rates.length - 1) * step;
    const points = rates.map((r, i) => `${(start + i * step).toFixed(1)},${(height - 2 - (pick(r) / peak) * (height - 6)).toFixed(1)}`);
    return svgNode("polyline", { class: `line ${cls}`, points: points.join(" ") });
  };
  graph.replaceChildren(...grid, ...(rates.length > 1 ? [line((r) => r.in, "line-in"), line((r) => r.out, "line-out")] : []));
  const last = rates[rates.length - 1];
  el("traffic-rates").textContent = last ? `${bytes(last.in)}/s in, ${bytes(last.out)}/s out` : "Measuring…";
  el("traffic-scale").textContent = rates.length ? `Top of graph ${bytes(peak)}/s · last 10 minutes` : "";
}

// The node's status changes only with a new block; the sync figures and the
// peer list move in between.
const REFRESH_MS = 5000;
setInterval(() => {
  if (!wasReady) return;
  void sampleTraffic();
  if (document.hidden) return;
  // The wallet too: its lock and staking state change between blocks.
  if (current === "home") {
    void refreshOverview();
    void refreshWallet();
  }
  if (current === "stake") void refreshStake();
  if (current === "network") void refreshNetwork();
}, REFRESH_MS);

// ---- setup wizard ----------------------------------------------------------

// Mirrors SetupInfo and bootstrap::Offer / Progress in src-tauri.
type BootstrapOffer = {
  height: number;
  size: number;
  sha256: string;
  url: string;
  block_hash: string | null;
  created: string | null;
};
type SetupInfo = {
  datadir: string;
  network: "main" | "regtest";
  offers: BootstrapOffer[];
  offers_error: string | null;
};
type BootstrapProgress =
  | { phase: "downloading"; received: number; size: number }
  | { phase: "verifying" }
  | { phase: "unpacking"; written: number }
  | { phase: "done"; height: number };

let setupChoice: "bootstrap" | "sync" | null = null;
let setupOffer: BootstrapOffer | null = null;
let setupLoaded = false;

function setupResult(tone: "info" | "danger", text: string): void {
  const box = el("setup-result");
  box.hidden = !text;
  bannerTone(box, tone);
  box.textContent = text;
}

function choose(choice: "bootstrap" | "sync"): void {
  setupChoice = choice;
  el("choose-bootstrap").setAttribute("aria-pressed", String(choice === "bootstrap"));
  el("choose-sync").setAttribute("aria-pressed", String(choice === "sync"));
  const go = el<HTMLButtonElement>("setup-go");
  go.disabled = false;
  go.textContent = choice === "bootstrap" ? "Download and install" : "Start syncing";
}

el("choose-bootstrap").addEventListener("click", () => choose("bootstrap"));
el("choose-sync").addEventListener("click", () => choose("sync"));

async function loadSetup(): Promise<void> {
  if (setupLoaded) return;
  setupLoaded = true;
  try {
    const info = await invoke<SetupInfo>("setup_info");
    el("setup-datadir").textContent = info.datadir;
    setupOffer = info.network === "main" ? (info.offers[0] ?? null) : null;
    const summary = el("bootstrap-summary");
    if (setupOffer) {
      const date = setupOffer.created ? `, made ${dateTime(Date.parse(setupOffer.created) / 1000)}` : "";
      summary.textContent = `Chain up to block ${count(setupOffer.height)}, ${bytes(setupOffer.size)} download${date}.`;
      el<HTMLButtonElement>("choose-bootstrap").disabled = false;
    } else if (info.network !== "main") {
      summary.textContent = "Bootstraps hold the mainnet chain; this node runs regtest.";
    } else {
      summary.textContent = info.offers_error ?? "The explorer offers no bootstrap right now.";
    }
  } catch (e) {
    setupLoaded = false;
    setupResult("danger", String(e));
  }
}

function showSetupProgress(p: BootstrapProgress): void {
  el("setup-choices").hidden = true;
  el("setup-progress").hidden = false;
  const size = setupOffer?.size ?? 0;
  let phase = "";
  let detail = "";
  let width = 0;
  switch (p.phase) {
    case "downloading":
      phase = "Downloading";
      detail = `${bytes(p.received)} of ${bytes(p.size)}`;
      width = p.size ? p.received / p.size : 0;
      break;
    case "verifying":
      phase = "Checking";
      detail = "Comparing the download with the explorer's SHA-256.";
      width = 1;
      break;
    case "unpacking":
      phase = "Unpacking";
      detail = `${bytes(p.written)} unpacked`;
      // Archives unpack to about 1.5 times their size.
      width = size ? Math.min(1, p.written / (size * 1.5)) : 0;
      break;
    case "done":
      phase = "Installed";
      detail = `Chain up to block ${count(p.height)} installed. Starting the node…`;
      width = 1;
      break;
  }
  el("setup-phase").textContent = phase;
  el("setup-detail").textContent = detail;
  el("setup-bar").style.width = `${width * 100}%`;
  el<HTMLButtonElement>("setup-cancel").disabled = p.phase === "done";
}

el("setup-go").addEventListener("click", async () => {
  const go = el<HTMLButtonElement>("setup-go");
  go.disabled = true;
  setupResult("info", "");
  try {
    if (setupChoice === "bootstrap" && setupOffer) {
      showSetupProgress({ phase: "downloading", received: 0, size: setupOffer.size });
      await invoke("setup_bootstrap", { height: setupOffer.height });
    } else if (setupChoice === "sync") {
      await invoke("setup_sync");
    }
  } catch (e) {
    el("setup-choices").hidden = false;
    el("setup-progress").hidden = true;
    setupResult("danger", String(e));
    go.disabled = false;
  }
});

el("setup-cancel").addEventListener("click", () => {
  el<HTMLButtonElement>("setup-cancel").disabled = true;
  void invoke("setup_cancel");
});

await listen<BootstrapProgress>("bootstrap-progress", (event) => showSetupProgress(event.payload));
await listen<string>("bootstrap-failed", (event) => {
  el("setup-choices").hidden = false;
  el("setup-progress").hidden = true;
  el<HTMLButtonElement>("setup-go").disabled = setupChoice === null;
  el<HTMLButtonElement>("setup-cancel").disabled = false;
  setupResult("danger", `${event.payload} Nothing was installed; you can try again or sync from the network.`);
});

// ---- settings --------------------------------------------------------------

// Mirrors settings::NodeSettings and SettingsView in src-tauri.
type NodeSettings = {
  dbcache_mib: number | null;
  script_threads: number | null;
  listen: boolean | null;
  natpmp: boolean | null;
  proxy: string | null;
  onion: string | null;
};
type SettingsView = { settings: NodeSettings; file: string; network: "main" | "regtest" };

// b3coind's defaults for the two flags.
const DEFAULT_LISTEN = true;
const DEFAULT_NATPMP = true;

let loadedSettings: NodeSettings | null = null;

function settingsResult(tone: "success" | "danger", text: string): void {
  const box = el("settings-result");
  box.hidden = !text;
  bannerTone(box, tone);
  box.textContent = text;
}

function syncProxyRows(): void {
  el("set-proxy-row").hidden = !el<HTMLInputElement>("set-proxy-use").checked;
  el("set-onion-row").hidden = !el<HTMLInputElement>("set-onion-use").checked;
}

function fillSettings(s: NodeSettings): void {
  el<HTMLInputElement>("set-dbcache").value = s.dbcache_mib === null ? "" : String(s.dbcache_mib);
  el<HTMLInputElement>("set-par").value = s.script_threads === null ? "" : String(s.script_threads);
  el<HTMLInputElement>("set-listen").checked = s.listen ?? DEFAULT_LISTEN;
  el<HTMLInputElement>("set-natpmp").checked = s.natpmp ?? DEFAULT_NATPMP;
  el<HTMLInputElement>("set-proxy-use").checked = s.proxy !== null;
  el<HTMLInputElement>("set-proxy").value = s.proxy ?? "";
  el<HTMLInputElement>("set-onion-use").checked = s.onion !== null;
  el<HTMLInputElement>("set-onion").value = s.onion ?? "";
  for (const id of ["set-dbcache", "set-par", "set-proxy", "set-onion"]) valid(el<HTMLInputElement>(id));
  syncProxyRows();
}

async function loadSettings(): Promise<void> {
  settingsResult("success", "");
  try {
    const view = await invoke<SettingsView>("node_settings");
    loadedSettings = view.settings;
    fillSettings(view.settings);
    el("settings-note").textContent =
      `The node stops and starts again with the new settings. They are kept in ${view.file}, ` +
      "which B3-Qt uses too, and take the place of the same options in b3coin.conf." +
      (view.network === "regtest"
        ? " This development node runs regtest, which keeps incoming connections off."
        : "");
  } catch (e) {
    loadedSettings = null;
    settingsResult("danger", String(e));
  }
}

/// A field the settings refused points at the message saying why.
function invalid(input: HTMLInputElement): void {
  input.setAttribute("aria-invalid", "true");
  input.setAttribute("aria-describedby", "settings-result");
}

function valid(input: HTMLInputElement): void {
  input.removeAttribute("aria-invalid");
  input.removeAttribute("aria-describedby");
}

/// An optional whole number from a field, or an error naming the field.
function wholeNumber(id: string, name: string): number | null {
  const input = el<HTMLInputElement>(id);
  const text = input.value.trim();
  valid(input);
  if (text === "") return null;
  if (!/^-?\d+$/.test(text)) {
    invalid(input);
    throw new Error(`${name} must be a whole number.`);
  }
  return Number(text);
}

function proxyField(useId: string, id: string, name: string): string | null {
  const input = el<HTMLInputElement>(id);
  valid(input);
  if (!el<HTMLInputElement>(useId).checked) return null;
  const text = input.value.trim();
  if (!text) {
    invalid(input);
    throw new Error(`Enter the ${name}, such as 127.0.0.1:9050.`);
  }
  return text;
}

/// A flag left unset stays unset while the box shows b3coind's default.
function flagField(id: string, loaded: boolean | null, fallback: boolean): boolean | null {
  const checked = el<HTMLInputElement>(id).checked;
  return loaded === null && checked === fallback ? null : checked;
}

function readSettingsForm(): NodeSettings {
  return {
    dbcache_mib: wholeNumber("set-dbcache", "The database cache"),
    script_threads: wholeNumber("set-par", "Script verification threads"),
    listen: flagField("set-listen", loadedSettings?.listen ?? null, DEFAULT_LISTEN),
    natpmp: flagField("set-natpmp", loadedSettings?.natpmp ?? null, DEFAULT_NATPMP),
    proxy: proxyField("set-proxy-use", "set-proxy", "proxy address"),
    onion: proxyField("set-onion-use", "set-onion", "Tor proxy address"),
  };
}

async function applySettings(settings: NodeSettings): Promise<void> {
  const save = el<HTMLButtonElement>("settings-save");
  const defaults = el<HTMLButtonElement>("settings-defaults");
  save.disabled = defaults.disabled = true;
  settingsResult("success", "");
  try {
    await invoke("node_settings_apply", { settings });
    settingsResult("success", "Saved. The node is starting again with these settings.");
    await loadSettings();
    settingsResult("success", "Saved. The node is starting again with these settings.");
  } catch (e) {
    settingsResult("danger", String(e));
  } finally {
    save.disabled = defaults.disabled = false;
  }
}

el("set-proxy-use").addEventListener("change", syncProxyRows);
el("set-onion-use").addEventListener("change", syncProxyRows);

/** What a restart interrupts, said before it happens. */
function restartConsequences(): string[] {
  const lines = [
    "The node stops and starts again with these settings. Balances and the other pages are unavailable until it is ready, which can take a few minutes.",
  ];
  if (stakingRunning)
    lines.push("Staking stops and does not start again on its own: start it again on the Stake page afterwards.");
  return lines;
}

el("settings-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  let settings: NodeSettings;
  try {
    settings = readSettingsForm();
  } catch (e) {
    settingsResult("danger", e instanceof Error ? e.message : String(e));
    return;
  }
  if (await confirmAction("Restart the node?", restartConsequences(), "Save and restart")) void applySettings(settings);
});

// Fills the form only; nothing changes until it is saved.
el("settings-defaults").addEventListener("click", () => {
  fillSettings({
    dbcache_mib: null,
    script_threads: null,
    listen: null,
    natpmp: null,
    proxy: null,
    onion: null,
  });
  // Flags left at their defaults are then saved as unset, as in a file
  // without them.
  if (loadedSettings) loadedSettings = { ...loadedSettings, listen: null, natpmp: null };
  settingsResult("success", "The form now shows the defaults. Save and restart to use them.");
});

// ---- send ------------------------------------------------------------------

function sendResult(tone: "info" | "success" | "warning" | "danger", text: string): void {
  const box = el("send-result");
  box.hidden = false;
  bannerTone(box, tone, "mt-4");
  box.textContent = text;
  // With several recipients the form is long; keep the outcome in view.
  box.scrollIntoView({ block: "nearest" });
}

// ---- PSBT ------------------------------------------------------------------
// Qt's PSBT operations: read a PSBT made elsewhere, sign it, broadcast it,
// copy or save it. Signing and broadcasting are approved in the approval
// window; the core reads the PSBT again there, so nothing the page shows is
// trusted.

type PsbtSummary = {
  chain: string;
  wallet: string;
  outputs: { address: string; amount: string; mine: boolean }[];
  fee: string | null;
  inputs: number;
  finalized: number;
  complete: boolean;
};
type PsbtView = { psbt: string; summary: PsbtSummary };

let psbtShown: PsbtView | null = null;
const psbtDrafts = new Set<string>();

function psbtResult(tone: "info" | "success" | "warning" | "danger", text: string): void {
  const box = el("psbt-result");
  box.hidden = false;
  bannerTone(box, tone, "mt-4");
  box.textContent = text;
  box.scrollIntoView({ block: "nearest" });
}

function renderPsbt(v: PsbtView | null): void {
  psbtShown = v;
  el("psbt-view").hidden = v === null;
  if (!v) return;
  const s = v.summary;
  el("psbt-outputs").replaceChildren(
    ...s.outputs.map((o) => {
      const r = amountRow(o.mine ? "To this wallet" : "Pay", o.amount);
      const line = document.createElement("span");
      line.className = "addr-line";
      line.textContent = o.address;
      r.querySelector(".dkey")!.append(line);
      return r;
    }),
  );
  el("psbt-rows").replaceChildren(
    s.fee === null ? textRow("Network fee", "Not in the PSBT") : amountRow("Network fee", s.fee),
    textRow("Inputs signed", `${s.finalized} of ${s.inputs}`),
    textRow("State", s.complete ? "Fully signed: ready to broadcast" : "Needs more signatures"),
  );
  el<HTMLButtonElement>("psbt-sign").hidden = s.complete;
  el<HTMLButtonElement>("psbt-send").hidden = !s.complete;
}

async function readPsbt(text: string): Promise<void> {
  el("psbt-result").hidden = true;
  try {
    const v = await invoke<PsbtView>("psbt_inspect", { psbt: text });
    el<HTMLTextAreaElement>("psbt-text").value = v.psbt;
    renderPsbt(v);
  } catch (e) {
    renderPsbt(null);
    psbtResult("danger", String(e));
  }
}

// A .psbt file is binary (Qt saves it so); a text file holds base64.
function psbtFileText(bytes: Uint8Array): string {
  const binary = bytes.length >= 5 && bytes[0] === 0x70 && bytes[1] === 0x73 && bytes[2] === 0x62 && bytes[3] === 0x74 && bytes[4] === 0xff;
  if (!binary) return new TextDecoder().decode(bytes);
  let s = "";
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  return btoa(s);
}

async function psbtDecided(r: DraftResult): Promise<void> {
  if (r.outcome === "signed") {
    await readPsbt(r.psbt);
    psbtResult(
      "success",
      r.complete
        ? "Signed. Every input is signed now, so it can be broadcast."
        : "Signed what this wallet could. It still needs other signatures: copy or save it for the next signer.",
    );
  } else if (r.outcome === "sent") {
    psbtResult("success", `Broadcast. Transaction ${r.txid}`);
    el<HTMLTextAreaElement>("psbt-text").value = "";
    renderPsbt(null);
    void refreshWallet();
    void refreshRecent();
  } else if (r.outcome === "rejected") {
    psbtResult("warning", "Rejected. Nothing was signed or sent.");
  } else if (r.outcome === "failed") {
    psbtResult("danger", `Not done: ${r.error}`);
  }
}

async function psbtDraft(send: boolean): Promise<void> {
  if (!psbtShown) return;
  try {
    const id = await invoke<string>("psbt_draft", { psbt: psbtShown.psbt, send });
    psbtDrafts.add(id);
    psbtResult("info", "Waiting for your decision in the approval window.");
  } catch (e) {
    psbtResult("danger", String(e));
  }
}

el("psbt-read").addEventListener("click", () => void readPsbt(el<HTMLTextAreaElement>("psbt-text").value));
el("psbt-text").addEventListener("input", () => renderPsbt(null));
el("psbt-open").addEventListener("click", () => el<HTMLInputElement>("psbt-file").click());
el("psbt-file").addEventListener("change", async () => {
  const input = el<HTMLInputElement>("psbt-file");
  const file = input.files?.[0];
  input.value = "";
  if (!file) return;
  void readPsbt(psbtFileText(new Uint8Array(await file.arrayBuffer())));
});
el("psbt-sign").addEventListener("click", () => void psbtDraft(false));
el("psbt-send").addEventListener("click", () => void psbtDraft(true));
el("psbt-copy").addEventListener("click", () => {
  if (!psbtShown) return;
  void navigator.clipboard.writeText(psbtShown.psbt).then(
    () => psbtResult("success", "PSBT copied."),
    () => psbtResult("danger", "The PSBT could not be copied."),
  );
});
el("psbt-save").addEventListener("click", async () => {
  if (!psbtShown) return;
  try {
    const path = await invoke<string | null>("psbt_save", { psbt: psbtShown.psbt });
    if (path !== null) psbtResult("success", `Saved to ${path}`);
  } catch (e) {
    psbtResult("danger", String(e));
  }
});

// ---- stake -------------------------------------------------------------------

el("staking-go").addEventListener("click", (e) => {
  e.preventDefault();
  go("stake");
});

// Which page asked for each waiting approval, so its outcome lands there.
const draftPage = new Map<string, "send" | "stake" | "finality" | "release">();

// Mirrors stake::Status in src-tauri/src/stake.rs.
type StakeEntry = {
  txid: string;
  vout: number;
  amount: string;
  status: "UNCONFIRMED" | "PENDING" | "ACTIVE";
  confirmations: number;
  active_at_height: number | null;
};
type StakeStatus = {
  validator_key: string | null;
  tip_height: number;
  phase: string;
  min_stake: string | null;
  activation_depth: number;
  active_weight: number;
  total_active_weight: number;
  stakes: StakeEntry[];
  active: string;
  pending: string;
  unconfirmed: string;
  loop: {
    running: boolean;
    state: string;
    last_error: string | null;
    ours: boolean;
    finality_signing: boolean;
    blocks_produced: number;
  };
  finality: { bound: boolean; revoked: boolean; key_is_ours: boolean | null; member: boolean; weight: number } | null;
  bind_pending: string | null;
  // Mirrors stake::Producers.
  producers: { frozen: boolean; stake_enters_at: number | null };
};

const PHASE_TEXT: Record<string, string> = {
  legacy: "Legacy era: stakes open after the B3 transition",
  corridor: "Transition corridor: staking begins after it",
  modern_pos: "Modern proof of stake",
};

type StakeStep = { title: string; detail: string; state: "done" | "active" | "pending" };

/** Whether staking can start: the node refuses it without a finality key
 *  this wallet holds (stake::start_error). A key that could not be checked,
 *  as while the wallet is locked, is checked when staking starts. */
function finalityReady(s: StakeStatus): boolean {
  const f = s.finality;
  return !!f && (f.member || (f.bound && !f.revoked && f.key_is_ours !== false));
}

/** What staking takes, in order, and how far this wallet is. A step that
 *  can be taken now is "active", one waiting on an earlier step "pending". */
function stakeSteps(s: StakeStatus): StakeStep[] {
  const open = s.phase === "modern_pos";
  const l = s.loop;
  const f = s.finality;
  const counting = s.active_weight > 0;
  // ACTIVE stake produces only once the producer set rotates to include it.
  const outside = !counting && s.stakes.some((k) => k.status === "ACTIVE");
  const enters = s.producers.stake_enters_at;
  const waiting = s.stakes
    .filter((k) => k.status !== "ACTIVE")
    .map((k) => k.active_at_height)
    .filter((h): h is number => h !== null)
    .sort((a, b) => a - b)[0];
  const stake: StakeStep = counting
    ? { title: "Your stake counts", detail: `${formatAmount(s.active, 2)} takes part in making blocks.`, state: "done" }
    : outside
      ? {
          title: "Your stake joins block production",
          detail:
            enters !== null && enters > s.tip_height
              ? `It is active, and makes blocks from block ${enters.toLocaleString()} at the earliest, ${plural(enters - s.tip_height, "block")} from now.`
              : s.producers.frozen
                ? "It is active, and makes blocks once the producer set can change again (see above)."
                : "It is active, and makes blocks once the validator set next rotates to include it.",
          state: "active",
        }
    : waiting !== undefined
      ? {
          title: "Your stake counts",
          detail: `From block ${waiting.toLocaleString()}, ${plural(Math.max(waiting - s.tip_height, 0), "block")} from now.`,
          state: "active",
        }
      : s.stakes.length
        ? { title: "Your stake counts", detail: "Your stake is waiting to confirm.", state: "active" }
        : {
            title: "Make a stake",
            detail: `Lock B3 below. It counts ${plural(s.activation_depth, "block")} after it confirms.`,
            state: open ? "active" : "pending",
          };
  const key: StakeStep = finalityReady(s)
    ? { title: "Finality key bound", detail: "This wallet signs the votes that make blocks final.", state: "done" }
    : s.bind_pending
      ? { title: "Finality key binding sent", detail: "It counts once it confirms in a block.", state: "active" }
    : !f
      ? { title: "Bind the finality key", detail: "Possible once your first stake confirms: it creates this wallet's validator.", state: "pending" }
      : {
          title: "Bind the finality key",
          detail: f.revoked
            ? "The bound key was revoked. Bind a new one below."
            : f.bound
              ? "The bound key is another wallet's. Rotate it below."
              : "Bind it below. This sends a small transaction.",
          state: "active",
        };
  const run: StakeStep =
    l.running && l.ours && l.finality_signing
      ? { title: "Staking is on", detail: `${plural(l.blocks_produced, "block")} produced so far.`, state: "done" }
      : l.running && l.ours
        ? { title: "Turn on finality signing", detail: "Staking runs without its finality signer. Stop it and start it again.", state: "active" }
        : l.running
          ? { title: "Start staking", detail: "This node stakes with another wallet. Stop it in that wallet first.", state: "pending" }
          : {
              title: "Start staking",
              detail: "A locked wallet unlocks only to start it, and locks again straight after.",
              state: open && finalityReady(s) ? "active" : "pending",
            };
  return [
    open
      ? { title: "Staking is open", detail: "The network accepts stakes.", state: "done" }
      : { title: "Staking opens", detail: PHASE_TEXT[s.phase] ?? s.phase, state: "pending" },
    stake,
    key,
    run,
  ];
}

const STEP_SPOKEN = { done: "Done", active: "To do", pending: "Later" } as const;

function renderSteps(steps: StakeStep[]): void {
  const list = el("stk-steps");
  list.hidden = steps.every((s) => s.state === "done");
  list.replaceChildren(
    ...steps.map((step, i) => {
      const item = document.createElement("li");
      item.className = `ci ${step.state}`;
      const mark = document.createElement("span");
      mark.className = "ci-mark";
      mark.setAttribute("aria-hidden", "true");
      if (step.state === "done") mark.append(icon("check", "i-sm"));
      else mark.textContent = String(i + 1);
      const body = document.createElement("div");
      body.className = "ci-body";
      const title = document.createElement("strong");
      const spoken = document.createElement("span");
      spoken.className = "sr-only";
      spoken.textContent = `${STEP_SPOKEN[step.state]}: `;
      title.append(spoken, step.title);
      const detail = document.createElement("span");
      detail.className = "t-sm muted";
      detail.textContent = step.detail;
      body.append(title, detail);
      item.append(mark, body);
      return item;
    }),
  );
}

function stakeResult(tone: "success" | "warning" | "danger", text: string): void {
  const box = el("stk-result");
  box.hidden = false;
  bannerTone(box, tone);
  box.textContent = text;
  box.scrollIntoView({ block: "nearest" });
}

function setBadge(id: string, text: string, tone: string): void {
  el(id).textContent = text;
  el(id).className = `badge badge-${tone}`;
}

function textRow(key: string, value: string, title?: string): HTMLElement {
  const drow = document.createElement("div");
  drow.className = "drow";
  const k = document.createElement("span");
  k.className = "dkey";
  k.textContent = key;
  const v = document.createElement("span");
  v.className = "dval";
  v.textContent = value;
  if (title) v.title = title;
  drow.append(k, v);
  return drow;
}

function amountRow(key: string, value: string): HTMLElement {
  const drow = textRow(key, "");
  drow.querySelector(".dval")!.replaceChildren(amountNode(value));
  return drow;
}

let stakeStatus: StakeStatus | null = null;

function renderStake(s: StakeStatus): void {
  const l = s.loop;
  if (l.running && l.ours) setBadge("stk-badge", l.finality_signing ? "On" : "On, finality not signing", l.finality_signing ? "success" : "warning");
  else if (l.running) setBadge("stk-badge", "Another wallet is staking", "warning");
  else setBadge("stk-badge", "Off", "neutral");
  el("stk-state").textContent = l.running
    ? l.state
    : s.phase === "modern_pos"
      ? "Staking is off. Your node makes blocks with your stakes while it is on."
      : "Staking is off.";
  el("stk-frozen").hidden = !s.producers.frozen;
  renderSteps(stakeSteps(s));
  const rows = [
    textRow("Network phase", PHASE_TEXT[s.phase] ?? s.phase),
    textRow("Current block", s.tip_height.toLocaleString()),
    textRow(
      "Your share",
      `${isMasked() ? "••••" : s.active_weight.toLocaleString()} of ${s.total_active_weight.toLocaleString()} B3`,
      "This wallet's counting stake, out of all counting stake on the network",
    ),
  ];
  if (l.running) rows.push(textRow("Blocks produced", l.blocks_produced.toLocaleString()));
  if (l.last_error) rows.push(textRow("Last error", l.last_error));
  if (s.validator_key) rows.push(textRow("Validator key", shorten(s.validator_key, 10, 8), s.validator_key));
  el("stk-rows").replaceChildren(...rows);
  el<HTMLButtonElement>("stk-start").hidden = l.running;
  el<HTMLButtonElement>("stk-stop").hidden = !l.running || !l.ours;
  // The checklist says what is missing.
  el<HTMLButtonElement>("stk-start").disabled = s.phase !== "modern_pos" || !finalityReady(s);
  el<HTMLButtonElement>("stk-stop").disabled = false;

  el("stk-count").textContent = s.stakes.length ? plural(s.stakes.length, "stake") : "";
  el("stk-totals").replaceChildren(
    amountRow("Counting", s.active),
    amountRow("Not counting yet", s.pending),
    ...(s.unconfirmed.replace(/[0.]/g, "") ? [amountRow("Unconfirmed", s.unconfirmed)] : []),
  );
  el("stk-empty").hidden = s.stakes.length > 0;
  el("stk-list").replaceChildren(
    ...s.stakes.map((k) => {
      const row = document.createElement("div");
      row.className = "list-row";
      const lead = document.createElement("div");
      lead.className = "lead stake";
      lead.append(icon("layers"));
      const body = document.createElement("div");
      body.className = "body";
      const meta = document.createElement("span");
      meta.className = "meta";
      meta.textContent =
        k.status === "UNCONFIRMED"
          ? "Waiting to confirm"
          : k.status === "PENDING"
            ? `Counts from block ${k.active_at_height?.toLocaleString() ?? "?"}`
            : plural(k.confirmations, "confirmation");
      body.append(amountNode(k.amount), meta);
      const tag = document.createElement("span");
      const tone = k.status === "ACTIVE" ? "success" : k.status === "PENDING" ? "info" : "neutral";
      tag.className = `badge badge-${tone}`;
      tag.textContent = k.status === "ACTIVE" ? "Active" : k.status === "PENDING" ? "Pending" : "Unconfirmed";
      row.append(lead, body, tag);
      if (k.status !== "UNCONFIRMED") {
        const release = document.createElement("button");
        release.type = "button";
        release.className = "btn btn-ghost btn-sm";
        release.textContent = "Release";
        release.title = "Return this stake to ordinary B3 in this wallet";
        release.addEventListener("click", () => void releaseStakes([{ txid: k.txid, vout: k.vout }]));
        row.append(release);
      }
      return row;
    }),
  );
  const releasable = s.stakes.filter((k) => k.status !== "UNCONFIRMED");
  el("stk-release-all-row").hidden = releasable.length < 2;
  el<HTMLButtonElement>("stk-release-all").disabled = false;
  el("stk-release-all").textContent = `Release all ${releasable.length}`;

  el("stk-min").textContent = s.min_stake
    ? `At least ${formatAmount(s.min_stake, 2)}. It counts ${s.activation_depth} blocks after it confirms.`
    : "This network has no minimum stake configured.";
  el<HTMLButtonElement>("stk-review").disabled = s.phase === "legacy" || !s.min_stake;

  const f = s.finality;
  const bind = el<HTMLButtonElement>("fin-bind");
  el("fin-text").title = "";
  if (s.bind_pending) {
    // A second binding before this one confirms could never confirm.
    setBadge("fin-badge", "Confirming", "info");
    el("fin-text").textContent = "A finality key binding from this wallet is waiting to confirm. You can bind again only after it does.";
    el("fin-text").title = `Transaction ${s.bind_pending}`;
    bind.disabled = true;
  } else if (!f) {
    setBadge("fin-badge", "No validator yet", "neutral");
    el("fin-text").textContent =
      "Validators sign the votes that make blocks final with this key. Your first stake creates this wallet's validator; bind its finality key after that.";
    bind.disabled = true;
    bind.textContent = "Bind finality key";
  } else if (f.bound && !f.revoked) {
    setBadge("fin-badge", f.member ? "Bound · in the validator set" : "Bound", "success");
    el("fin-text").textContent =
      f.key_is_ours === false
        ? "The bound key is not this wallet's. Rotate it to bind this wallet's key."
        : f.key_is_ours === true
          ? "This wallet holds the bound key. Rotating replaces it at the next epoch boundary."
          : "Ownership of the bound key could not be checked, for example while the wallet is locked. Starting staking checks it.";
    bind.disabled = false;
    bind.textContent = "Rotate finality key";
  } else {
    setBadge("fin-badge", f.revoked ? "Revoked" : "Not bound", "warning");
    el("fin-text").textContent =
      "Validators sign the votes that make blocks final with this key, and staking needs it. Binding sends a small transaction.";
    bind.disabled = false;
    bind.textContent = "Bind finality key";
  }
}

// Bumped whenever the wallet changes, so a slow reply for the old wallet is dropped.
let stakeGeneration = 0;

function clearStake(): void {
  stakeGeneration++;
  stakeStatus = null;
  stakeReadAt = null;
  el("view-stake").classList.remove("is-stale");
  el("stk-stale").hidden = true;
  for (const id of ["stk-start", "stk-stop"]) {
    const b = el<HTMLButtonElement>(id);
    b.hidden = true;
    b.disabled = true;
  }
  el<HTMLButtonElement>("fin-bind").disabled = true;
  el("stk-rows").replaceChildren();
  el("stk-steps").replaceChildren();
  el("stk-steps").hidden = true;
  el("stk-frozen").hidden = true;
  el("stk-list").replaceChildren();
  el("stk-totals").replaceChildren();
  el("stk-count").textContent = "";
  el("stk-state").textContent = "Loading this wallet's staking…";
  setBadge("fin-badge", "Loading", "neutral");
  el("fin-text").textContent = "";
  el("stk-result").hidden = true;
}

let stakeRefreshAgain = false;
let stakeRefreshing = false;
let stakeReadAt: number | null = null;

/** A failed read keeps the last one up, marked with its age, and turns its
 *  controls off: they act on what the page last saw. The next good read
 *  turns them on again. */
function stakeStale(error: string): void {
  const note = el("stk-stale");
  note.hidden = false;
  note.textContent =
    stakeReadAt === null
      ? "Staking could not be read. Trying again…"
      : `Not updated since ${readTime(stakeReadAt)}: staking could not be read. Its controls are off until it is. Trying again…`;
  note.title = error;
  el("view-stake").classList.add("is-stale");
  for (const b of el("view-stake").querySelectorAll<HTMLButtonElement>("button")) b.disabled = true;
}
async function refreshStake(): Promise<void> {
  // A request that arrives while one is in flight (for example right after a
  // wallet switch) runs again afterwards instead of being dropped.
  if (stakeRefreshing) {
    stakeRefreshAgain = true;
    return;
  }
  stakeRefreshing = true;
  try {
    do {
      stakeRefreshAgain = false;
      const generation = stakeGeneration;
      let status: StakeStatus;
      try {
        status = await invoke<StakeStatus>("stake_status");
      } catch (e) {
        if (generation === stakeGeneration) stakeStale(String(e));
        continue;
      }
      if (generation !== stakeGeneration) {
        stakeRefreshAgain = true;
        continue;
      }
      stakeStatus = status;
      stakeReadAt = Date.now();
      el("view-stake").classList.remove("is-stale");
      el("stk-stale").hidden = true;
      renderStake(status);
    } while (stakeRefreshAgain);
  } finally {
    stakeRefreshing = false;
  }
}

// Releasing (unstaking) goes through the approval window, like a payment.
/** The release being decided takes every stake this wallet can release. */
let releasingAll = false;

async function releaseStakes(outpoints: { txid: string; vout: number }[]): Promise<void> {
  try {
    const id = await invoke<string>("release_draft", { outpoints });
    draftPage.set(id, "release");
    releasingAll =
      outpoints.length === (stakeStatus?.stakes ?? []).filter((k) => k.status !== "UNCONFIRMED").length;
    stakeResult("success", "Waiting for your decision in the approval window.");
  } catch (e) {
    stakeResult("danger", String(e));
  }
}

/** After the whole stake is released, staking is offered to be stopped;
 *  block production never spends stakes, so it need not be for safety. */
function stakingAfterRelease(): string {
  const l = stakeStatus?.loop;
  if (!releasingAll || !l?.running || !l.ours) return "";
  return stakeStatus?.producers.frozen
    ? " Staking is still on: while the producer set is frozen your validator keeps its place and still makes blocks. Stop staking above if you no longer want it to."
    : " Staking is still on. With no stake left you can stop it above.";
}

el("stk-release-all").addEventListener("click", () => {
  const all = (stakeStatus?.stakes ?? []).filter((k) => k.status !== "UNCONFIRMED");
  void releaseStakes(all.map((k) => ({ txid: k.txid, vout: k.vout })));
});

el("stk-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  const review = el<HTMLButtonElement>("stk-review");
  review.disabled = true;
  try {
    const id = await invoke<string>("stake_draft", { amount: el<HTMLInputElement>("stk-amount").value });
    draftPage.set(id, "stake");
    stakeResult("success", "Waiting for your decision in the approval window.");
  } catch (e) {
    stakeResult("danger", String(e));
  } finally {
    review.disabled = false;
  }
});

el("fin-bind").addEventListener("click", async () => {
  try {
    const id = await invoke<string>("finality_draft");
    draftPage.set(id, "finality");
    stakeResult("success", "Waiting for your decision in the approval window.");
  } catch (e) {
    stakeResult("danger", String(e));
  }
});

// An encrypted wallet unlocks in the passphrase window, only to start.
el("stk-start").addEventListener("click", async () => {
  try {
    const v = await invoke<WalletsView>("wallets");
    if (v.security === "locked" || v.security === "unlocked") {
      await invoke("passphrase_request", { kind: "stake" });
      return;
    }
    await invoke("stake_start");
    stakeResult("success", "Staking started.");
  } catch (e) {
    stakeResult("danger", String(e));
  }
  void refreshStake();
});

el("stk-stop").addEventListener("click", async () => {
  try {
    await invoke("stake_stop");
    stakeResult("success", "Staking stopped.");
  } catch (e) {
    stakeResult("danger", String(e));
  }
  void refreshStake();
});

// ---- address book ----------------------------------------------------------

// Mirrors book::Contact in src-tauri/src/book.rs.
type Contact = { address: string; label: string };
let contacts: Contact[] = [];

function bookResult(tone: "success" | "danger", text: string): void {
  const box = el("book-result");
  box.hidden = false;
  bannerTone(box, tone);
  box.textContent = text;
  box.scrollIntoView({ block: "nearest" });
}

// Starts a payment to this address on Send. While a payment to several
// recipients is being written, it goes in the first empty one, or a new one.
function payTo(c: Contact): void {
  go("send");
  const blocks = [...el("recipients").querySelectorAll<HTMLElement>(".recipient")];
  let block: HTMLElement | undefined;
  if (blocks.length > 1) {
    block =
      blocks.find((b) => !b.querySelector<HTMLInputElement>(".r-address")!.value.trim()) ?? addRecipient();
  } else {
    el("recipients").replaceChildren();
    block = addRecipient();
  }
  block.querySelector<HTMLInputElement>(".r-address")!.value = c.address;
  block.querySelector<HTMLInputElement>(".r-label")!.value = c.label;
  block.querySelector<HTMLInputElement>(".r-amount")!.focus();
}

function editContact(c: Contact): void {
  el<HTMLInputElement>("book-address").value = c.address;
  el<HTMLInputElement>("book-label").value = c.label;
  el("book-form-title").textContent = "Rename an address";
  el("book-form").scrollIntoView({ block: "nearest" });
  el<HTMLInputElement>("book-label").focus();
}

const refreshBook = single(async (fresh) => {
  try {
    const list = await invoke<Contact[]>("book_list");
    if (!fresh()) return;
    contacts = list;
  } catch (e) {
    if (fresh() && current === "book") bookResult("danger", String(e));
    return;
  }
  el("book-options").replaceChildren(
    ...contacts.map((c) => {
      const o = document.createElement("option");
      o.value = c.address;
      o.label = c.label || "(no label)";
      return o;
    }),
  );
  el("book-count").textContent = contacts.length ? plural(contacts.length, "address", "addresses") : "";
  el("book-empty").hidden = contacts.length > 0;
  el("book-list").replaceChildren(
    ...contacts.map((c) => {
      const row = document.createElement("div");
      row.className = "list-row";
      const lead = document.createElement("div");
      lead.className = "lead out";
      lead.append(icon("arrow-up"));
      const body = document.createElement("div");
      body.className = "body";
      const title = document.createElement("span");
      title.textContent = c.label || "(no label)";
      const meta = document.createElement("span");
      meta.className = "meta is-mono";
      meta.textContent = shorten(c.address, 14, 8);
      meta.title = c.address;
      body.append(title, meta);
      const pay = document.createElement("button");
      pay.type = "button";
      pay.className = "btn btn-secondary btn-sm";
      pay.textContent = "Pay";
      pay.addEventListener("click", () => payTo(c));
      const rename = document.createElement("button");
      rename.type = "button";
      rename.className = "btn btn-ghost btn-sm";
      rename.textContent = "Rename";
      rename.addEventListener("click", () => editContact(c));
      const copy = document.createElement("button");
      copy.type = "button";
      copy.className = "btn btn-ghost btn-icon btn-sm";
      copy.setAttribute("aria-label", "Copy address");
      copy.title = "Copy address";
      copy.append(icon("copy", "i-sm"));
      copyButton(copy, () => c.address);
      const trail = document.createElement("div");
      trail.className = "row";
      trail.append(copy, rename, pay);
      row.append(lead, body, trail);
      return row;
    }),
  );
});

el("book-form").addEventListener("reset", () => {
  el("book-form-title").textContent = "Save an address";
});

el("book-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  const save = el<HTMLButtonElement>("book-save");
  save.disabled = true;
  try {
    const c = await invoke<Contact>("book_save", {
      address: el<HTMLInputElement>("book-address").value,
      label: el<HTMLInputElement>("book-label").value,
    });
    el<HTMLFormElement>("book-form").reset();
    bookResult("success", `Saved ${c.label || "the address"}.`);
    await refreshBook();
  } catch (e) {
    bookResult("danger", String(e));
  } finally {
    save.disabled = false;
  }
});

// ---- send coin control -----------------------------------------------------

// Mirrors wallet::Coin in src-tauri/src/wallet.rs.
// Mirrors wallet::Coin and wallet::CoinList in src-tauri/src/wallet.rs.
type Coin = {
  txid: string;
  vout: number;
  address: string;
  label: string;
  amount: string;
  confirmations: number;
  // What spending it adds to a transaction, in virtual bytes.
  vbytes: number;
};
type CoinList = { coins: Coin[]; input_budget: number };

const MAX_RECIPIENTS = 50; // wallet::MAX_RECIPIENTS
let recipientSeq = 0;

function field(id: string, label: string, input: HTMLElement, help?: string): HTMLElement {
  const f = document.createElement("div");
  f.className = "field";
  const l = document.createElement("label");
  l.htmlFor = id;
  l.textContent = label;
  f.append(l, input);
  if (help) {
    const h = document.createElement("span");
    h.className = "help";
    h.textContent = help;
    f.append(h);
  }
  return f;
}

function textInput(id: string, cls: string, attrs: Record<string, string> = {}): HTMLInputElement {
  const i = document.createElement("input");
  i.id = id;
  i.className = `input ${cls}`;
  i.autocomplete = "off";
  i.spellcheck = false;
  for (const [k, v] of Object.entries(attrs)) i.setAttribute(k, v);
  return i;
}

// Numbers the recipients and shows Remove only when there is more than one.
function renumberRecipients(): void {
  const blocks = [...el("recipients").querySelectorAll<HTMLElement>(".recipient")];
  blocks.forEach((b, i) => {
    b.querySelector(".recipient-head")!.toggleAttribute("hidden", blocks.length < 2);
    b.querySelector(".r-title")!.textContent = `Recipient ${i + 1}`;
  });
  el("send-add").textContent = "";
  el("send-add").append(
    icon("plus", "i-sm"),
    blocks.length < 2 ? "Pay someone else in the same transaction" : "Add another recipient",
  );
  el<HTMLButtonElement>("send-add").disabled = blocks.length >= MAX_RECIPIENTS;
}

function addRecipient(): HTMLElement {
  const n = ++recipientSeq;
  const block = document.createElement("div");
  block.className = "recipient";

  const head = document.createElement("div");
  head.className = "recipient-head";
  const title = document.createElement("span");
  title.className = "r-title t-sm muted";
  const remove = document.createElement("button");
  remove.type = "button";
  remove.className = "btn btn-ghost btn-sm";
  remove.textContent = "Remove";
  remove.addEventListener("click", () => {
    block.remove();
    renumberRecipients();
  });
  head.append(title, remove);

  const amount = textInput(`r-amount-${n}`, "r-amount", { inputmode: "decimal", placeholder: "0.000000000" });
  const amountBox = document.createElement("div");
  amountBox.className = "amount-field";
  const unit = document.createElement("span");
  unit.className = "unit";
  unit.textContent = "B3";
  amountBox.append(amount, unit);

  const subtract = document.createElement("label");
  subtract.className = "check";
  const box = document.createElement("input");
  box.type = "checkbox";
  box.className = "r-subtract";
  const body = document.createElement("span");
  body.className = "check-body";
  const what = document.createElement("span");
  what.textContent = "Take the network fee from this amount";
  const help = document.createElement("span");
  help.className = "help";
  help.textContent = "The recipient gets a little less, instead of the fee being added on top.";
  body.append(what, help);
  subtract.append(box, body);

  // Saved addresses are offered as the address is typed, and bring their label.
  const address = textInput(`r-address-${n}`, "input-mono r-address", {
    list: "book-options",
    placeholder: "Paste an address, or choose one",
  });
  address.addEventListener("change", () => {
    const label = block.querySelector<HTMLInputElement>(".r-label")!;
    const saved = contacts.find((c) => c.address === address.value.trim());
    if (saved && !label.value.trim()) label.value = saved.label;
  });
  const choose = document.createElement("button");
  choose.type = "button";
  choose.className = "btn btn-secondary";
  choose.append(icon("book", "i-sm"), "Choose");
  choose.addEventListener("click", async () => {
    const picked = await pickAddress(
      "Pay to",
      "A saved contact, or one of your own addresses to move coins to.",
      "book",
    );
    if (!picked) return;
    address.value = picked.address;
    // The label is saved with the address once paid; an own address keeps
    // its own label, so none is filled in for it.
    block.querySelector<HTMLInputElement>(".r-label")!.value = picked.mine ? "" : picked.label;
    amount.focus();
  });
  const addressBox = document.createElement("div");
  addressBox.className = "pick-field";
  addressBox.append(address, choose);
  block.append(
    head,
    field(`r-address-${n}`, "Pay to", addressBox),
    field(
      `r-label-${n}`,
      "Label",
      textInput(`r-label-${n}`, "r-label", { maxlength: "200", placeholder: "Optional" }),
      "Saved in your address book once the payment is sent.",
    ),
    field(`r-amount-${n}`, "Amount", amountBox, "Up to 9 decimals."),
    subtract,
  );
  el("recipients").append(block);
  renumberRecipients();
  return block;
}

function resetSendForm(): void {
  el("recipients").replaceChildren();
  addRecipient();
  el<HTMLInputElement>("send-fee").value = "";
  chosenCoins.clear();
  el<HTMLInputElement>("send-cc").checked = false;
  el("send-coins").hidden = true;
  el<HTMLDetailsElement>("send-options").open = false;
}

el("send-add").addEventListener("click", () => {
  addRecipient().querySelector<HTMLInputElement>(".r-address")!.focus();
});

// Coin control. Chosen coins are kept by outpoint, so a refresh keeps the
// choice for coins that are still there.
const chosenCoins = new Set<string>();
let shownCoins: Coin[] = [];
const outpoint = (c: { txid: string; vout: number }): string => `${c.txid}:${c.vout}`;

// How many virtual bytes of coins fit in one transaction to one address
// plus change; the core checks the real payment again.
let inputBudget = 0;
// Wallets with thousands of small staking rewards are why the bulk choices
// exist: picking them one by one is not practical, and one transaction
// holds only so many (about 675 of the common kind).
const QUICK_COUNT = 50;
const LIST_LIMIT = 300;

const chosenList = (): Coin[] => shownCoins.filter((c) => chosenCoins.has(outpoint(c)));
const units = (c: Coin): bigint => amountUnits(c.amount) ?? 0n;
const ascending = (a: Coin, b: Coin): number => (units(a) < units(b) ? -1 : units(a) > units(b) ? 1 : 0);

function showCoinSum(): void {
  const picked = chosenList();
  const total = sumAmounts(picked.map((c) => c.amount)) ?? "0";
  el("send-coins-sum").replaceChildren(
    amountNode(total),
    document.createTextNode(` · ${plural(picked.length, "coin")}`),
  );
  const size = picked.reduce((n, c) => n + c.vbytes, 0);
  const over = size > inputBudget;
  el("cc-size").hidden = picked.length === 0;
  el("cc-size").className = over ? "help bad" : "help";
  el("cc-size").textContent = over
    ? `Too many for one transaction: about ${Math.ceil(size / 1000)} kB of coins, and about ${Math.floor(inputBudget / 1000)} kB fit. Choose fewer and combine the rest in another payment.`
    : `About ${Math.ceil(size / 1000)} of ${Math.floor(inputBudget / 1000)} kB of one transaction.`;
  el<HTMLButtonElement>("cc-combine").disabled = picked.length === 0 || over;
}

// Takes coins in order while they fit in one transaction, up to `count`.
function fitting(ordered: Coin[], count = Infinity): Coin[] {
  const out: Coin[] = [];
  let size = 0;
  for (const c of ordered) {
    if (out.length >= count || size + c.vbytes > inputBudget) break;
    size += c.vbytes;
    out.push(c);
  }
  return out;
}

function chooseCoins(coins: Coin[], note?: string): void {
  chosenCoins.clear();
  for (const c of coins) chosenCoins.add(outpoint(c));
  renderCoins();
  if (note) sendResult("info", note);
}

function renderCoins(): void {
  const many = shownCoins.length > QUICK_COUNT;
  el("cc-tools").hidden = shownCoins.length < 2;
  el("cc-search").hidden = shownCoins.length <= 10;
  el("cc-help").textContent = many
    ? `${shownCoins.length.toLocaleString()} coins. Choose them in bulk here instead of one by one. One transaction holds about ${Math.floor(inputBudget / 148)} of the common kind; combine the rest in another payment.`
    : "Choose coins in bulk, or tick them below.";
  const q = el<HTMLInputElement>("cc-search").value.trim().toLowerCase();
  const matching = q
    ? shownCoins.filter((c) => [c.address, c.label, c.amount].some((t) => t.toLowerCase().includes(q)))
    : shownCoins;
  // Chosen coins first, so a bulk choice shows what it took.
  const ordered = [...matching.filter((c) => chosenCoins.has(outpoint(c))), ...matching.filter((c) => !chosenCoins.has(outpoint(c)))];
  const listed = ordered.slice(0, LIST_LIMIT);
  el("cc-more").hidden = listed.length === matching.length;
  el("cc-more").textContent = `Showing ${listed.length} of ${matching.length.toLocaleString()} coins. Search, or use the buttons above to choose among all of them.`;
  el("send-coins-empty").hidden = shownCoins.length > 0;
  el("send-coin-list").replaceChildren(
    ...listed.map((c) => {
      const row = document.createElement("label");
      row.className = "list-row";
      const box = document.createElement("input");
      box.type = "checkbox";
      box.checked = chosenCoins.has(outpoint(c));
      box.addEventListener("change", () => {
        if (box.checked) chosenCoins.add(outpoint(c));
        else chosenCoins.delete(outpoint(c));
        showCoinSum();
      });
      const body = document.createElement("span");
      body.className = "body";
      const amount = amountNode(c.amount);
      const meta = document.createElement("span");
      meta.className = "meta is-mono";
      const address = c.address || "No address (pay to public key)";
      meta.textContent = c.label ? `${c.label} · ${shorten(address, 10, 6)}` : shorten(address, 14, 8);
      meta.title = address;
      body.append(amount, meta);
      const trail = document.createElement("span");
      trail.className = "trail t-sm muted";
      trail.textContent = c.confirmations ? plural(c.confirmations, "confirmation") : "Unconfirmed";
      row.append(box, body, trail);
      return row;
    }),
  );
  showCoinSum();
}

const refreshCoins = single(async (current) => {
  if (!el<HTMLInputElement>("send-cc").checked) return;
  try {
    const list = await invoke<CoinList>("send_coins");
    if (!current()) return;
    shownCoins = list.coins;
    inputBudget = list.input_budget;
  } catch (e) {
    if (current()) sendResult("danger", String(e));
    return;
  }
  const present = new Set(shownCoins.map(outpoint));
  for (const key of [...chosenCoins]) if (!present.has(key)) chosenCoins.delete(key);
  renderCoins();
});

el("cc-search").addEventListener("input", () => renderCoins());
el("cc-clear").addEventListener("click", () => chooseCoins([]));
el("cc-smallest").addEventListener("click", () => chooseCoins(fitting([...shownCoins].sort(ascending), QUICK_COUNT)));
el("cc-largest").addEventListener("click", () =>
  chooseCoins(fitting([...shownCoins].sort(ascending).reverse(), QUICK_COUNT)),
);
el("cc-fit").addEventListener("click", () => {
  const picked = fitting([...shownCoins].sort(ascending));
  chooseCoins(
    picked,
    picked.length < shownCoins.length
      ? `Chose the ${picked.length.toLocaleString()} smallest coins that fit in one transaction; ${(shownCoins.length - picked.length).toLocaleString()} are left for another.`
      : undefined,
  );
});

// The fewest, largest coins that cover the amounts typed, with a margin for
// the fee; the wallet works out the real fee.
el("cc-enough").addEventListener("click", () => {
  const amounts = [...el("recipients").querySelectorAll<HTMLInputElement>(".r-amount")].map((i) => i.value.trim());
  const total = amounts.every((a) => a !== "") ? sumAmounts(amounts) : null;
  const need = total === null ? null : amountUnits(total);
  if (need === null || need <= 0n) {
    sendResult("danger", "Enter the amounts first.");
    return;
  }
  const margin = 100_000n; // 0.0001 B3
  const picked: Coin[] = [];
  let have = 0n;
  for (const c of fitting([...shownCoins].sort(ascending).reverse())) {
    if (have >= need + margin) break;
    picked.push(c);
    have += units(c);
  }
  if (have < need + margin) {
    chooseCoins([]);
    sendResult("danger", "The coins that fit in one transaction do not add up to that amount.");
    return;
  }
  chooseCoins(picked);
});

// Combining many small coins into one: the form pays their total to a new
// address of this wallet, with the fee taken from it. Nothing is sent until
// it is reviewed and approved.
el("cc-combine").addEventListener("click", async () => {
  const picked = chosenList();
  const total = sumAmounts(picked.map((c) => c.amount));
  if (!picked.length || total === null) return;
  try {
    const to = await invoke<{ address: string }>("receive_new", { label: "Combined coins", amount: "", message: "" });
    el("recipients").replaceChildren();
    const block = addRecipient();
    block.querySelector<HTMLInputElement>(".r-address")!.value = to.address;
    block.querySelector<HTMLInputElement>(".r-amount")!.value = total;
    block.querySelector<HTMLInputElement>(".r-subtract")!.checked = true;
    sendResult(
      "info",
      `The form now pays the ${plural(picked.length, "chosen coin")} to a new address of this wallet, labelled "Combined coins", with the fee taken from the amount. Review the payment to combine them.`,
    );
    el<HTMLFormElement>("send").scrollIntoView({ block: "start" });
  } catch (e) {
    sendResult("danger", String(e));
  }
});

el("send-cc").addEventListener("change", () => {
  const on = el<HTMLInputElement>("send-cc").checked;
  el("send-coins").hidden = !on;
  if (on) void refreshCoins();
});

resetSendForm();

// The page only asks for a draft. The core funds it, the approval window
// shows it, and it is signed and sent only if approved there.
el("send").addEventListener("submit", async (event) => {
  event.preventDefault();
  const coinControl = el<HTMLInputElement>("send-cc").checked;
  if (coinControl && chosenCoins.size === 0) {
    sendResult("danger", "Choose at least one coin, or turn off choosing coins.");
    return;
  }
  const recipients = [...el("recipients").querySelectorAll<HTMLElement>(".recipient")].map((b) => ({
    address: b.querySelector<HTMLInputElement>(".r-address")!.value.trim(),
    amount: b.querySelector<HTMLInputElement>(".r-amount")!.value.trim(),
    label: b.querySelector<HTMLInputElement>(".r-label")!.value.trim(),
    subtract_fee: b.querySelector<HTMLInputElement>(".r-subtract")!.checked,
  }));
  const coins = coinControl
    ? shownCoins.filter((c) => chosenCoins.has(outpoint(c))).map((c) => ({ txid: c.txid, vout: c.vout }))
    : [];
  const submit = el<HTMLButtonElement>("send-submit");
  submit.disabled = true;
  sendResult("info", "Preparing the payment…");
  try {
    const id = await invoke<string>("draft_send", {
      payment: { recipients, fee_rate: el<HTMLInputElement>("send-fee").value.trim(), coins },
    });
    draftPage.set(id, "send");
    sendResult("info", "Waiting for your decision in the approval window.");
    void refreshCoins();
  } catch (e) {
    sendResult("danger", String(e));
  } finally {
    submit.disabled = false;
  }
});

// ---- updates ---------------------------------------------------------------

// Mirrors UpdateStatus in src-tauri/src/update.rs; the helper names the state.
type Offer = { version: string; size: number; sha256: string; url: string; notes_sha256: string };
type UpdateStatus = {
  state: string;
  error: string;
  installed: string;
  offer: Offer | null;
  download: string | null;
};

const STATE_TEXT: Record<string, [string, string]> = {
  unconfigured: ["No update channel", "neutral"],
  idle: ["Not checked", "neutral"],
  checking: ["Checking", "info"],
  "update-available": ["Update available", "accent"],
  downloading: ["Downloading", "info"],
  "ready-to-install": ["Downloaded and verified", "success"],
  failed: ["Failed", "danger"],
};

// The helper reports stable tokens; the common ones get words.
const ERROR_TEXT: Record<string, string> = {
  "update-fetch-dns": "The release server could not be found. Check your internet connection.",
  "update-fetch-connect": "The release server could not be reached.",
  "update-fetch-timeout": "The release server took too long to answer.",
  "update-fetch-tls": "The release server's HTTPS certificate was not accepted.",
  "update-fetch-redirect": "The release server sent the download somewhere B3 Hive does not trust.",
  "update-fetch-oversize": "The release server sent more data than the release allows.",
  "update-reject-expired": "The published release list has expired.",
  "update-reject-rollback": "The published release list is older than one already seen.",
};

function updateError(token: string): string {
  if (ERROR_TEXT[token]) return `${ERROR_TEXT[token]} (${token})`;
  if (token.startsWith("update-fetch-http-")) return `The release server answered HTTP ${token.slice(18)}.`;
  return token ? `The update was refused: ${token}` : "";
}

function megabytes(bytes: number): string {
  return `${(bytes / 1_000_000).toLocaleString("en-US", { maximumFractionDigits: 1 })} MB`;
}

function updateResult(tone: "info" | "success" | "warning" | "danger", text: string): void {
  const box = el("update-result");
  box.hidden = !text;
  bannerTone(box, tone, "mt-4");
  box.textContent = text;
}

let offerSize = 0;
// An idle answer after a check means nothing newer was offered.
let checked = false;

function renderUpdates(u: UpdateStatus): void {
  const upToDate = u.state === "idle" && u.offer === null && checked;
  const [text, tone] = upToDate ? ["Up to date", "success"] : (STATE_TEXT[u.state] ?? [u.state, "neutral"]);
  const state = el("update-state");
  state.textContent = text;
  state.className = `badge badge-${tone}`;
  el("update-installed").textContent = u.installed;
  el("update-offer").hidden = !u.offer;
  offerSize = u.offer?.size ?? 0;
  if (u.offer) {
    el("offer-version").textContent = u.offer.version;
    el("offer-size").textContent = megabytes(u.offer.size);
    el("offer-sha").textContent = shorten(u.offer.sha256, 12, 12);
    el("offer-sha").title = u.offer.sha256;
    el("offer-url").textContent = u.offer.url;
  }
  el("update-check").hidden = u.state === "unconfigured";
  el("update-download").hidden = u.state !== "update-available";
  el("update-progress").hidden = u.state !== "ready-to-install";
  el("update-progress").classList.add("progress--ok");
  el("update-progress-bar").style.width = "100%";
  if (u.state === "unconfigured") {
    updateResult("info", "This build of B3 Hive has no pinned release channel, so it never looks for updates.");
  } else if (u.state === "failed") {
    updateResult("danger", updateError(u.error));
  } else if (u.state === "ready-to-install" && u.download) {
    updateResult("success", `Saved to ${u.download}. Close B3 Hive and run that file to install ${u.offer?.version ?? "it"}.`);
  } else if (upToDate) {
    updateResult("success", `B3 Hive ${u.installed} is the latest release.`);
  } else {
    updateResult("info", "");
  }
}

// One update command runs at a time, so both buttons wait for it.
async function updateCommand(command: string, busy: string): Promise<void> {
  const buttons = [el<HTMLButtonElement>("update-check"), el<HTMLButtonElement>("update-download")];
  for (const b of buttons) b.disabled = true;
  updateResult("info", busy);
  try {
    renderUpdates(await invoke<UpdateStatus>(command));
  } catch (e) {
    updateResult("danger", String(e));
  } finally {
    for (const b of buttons) b.disabled = false;
  }
}

async function refreshUpdates(): Promise<void> {
  try {
    renderUpdates(await invoke<UpdateStatus>("update_status"));
  } catch (e) {
    updateResult("danger", String(e));
  }
}

el("update-check").addEventListener("click", async () => {
  checked = true;
  await updateCommand("update_check", "Checking the signed release list…");
});

el("update-download").addEventListener("click", async () => {
  const bar = el("update-progress");
  bar.hidden = false;
  bar.classList.remove("progress--ok");
  el("update-progress-bar").style.width = "0";
  await updateCommand("update_download", "Downloading and verifying the release…");
});

await listen<number>("update-progress", (event) => {
  if (offerSize > 0) el("update-progress-bar").style.width = `${Math.min(100, (event.payload / offerSize) * 100)}%`;
});

// Mirrors PassResult in src-tauri/src/lib.rs: only the outcome, never the
// passphrase.
type PassResult =
  | { outcome: "done"; kind: "encrypt" | "change" | "stake"; wallet: string }
  | { outcome: "cancelled"; kind: "encrypt" | "change" | "stake" };

await listen<PassResult>("passphrase-result", (event) => {
  const r = event.payload;
  if (r.kind === "stake") {
    if (r.outcome === "done") stakeResult("success", "Staking started. The wallet is locked again; staking goes on.");
    void refreshStake();
    return;
  }
  if (r.outcome === "done") {
    walletsResult(
      "success",
      r.kind === "encrypt"
        ? `${walletName(r.wallet)} is now encrypted. Make a new backup now: encrypting started a new key chain, so older backups do not hold the keys for new addresses.`
        : `The passphrase of ${walletName(r.wallet)} was changed.`,
    );
  }
  void refreshWallets();
  void refreshWallet();
});

await listen<DraftResult>("draft-result", (event) => {
  const r = event.payload;
  if (tradeDecided(r as TradeResult)) return;
  if (psbtDrafts.has(r.id)) {
    psbtDrafts.delete(r.id);
    void psbtDecided(r);
    return;
  }
  const page = draftPage.get(r.id) ?? "send";
  draftPage.delete(r.id);
  if (page !== "send") {
    if (r.outcome === "sent") {
      stakeResult(
        "success",
        page === "stake"
          ? `Stake sent. Transaction ${r.txid}. It counts once it confirms and matures.`
          : page === "release"
            ? `Release sent. Transaction ${r.txid}. The B3 is ordinary coins in this wallet once it confirms.${stakingAfterRelease()}`
            : `Finality key sent. Transaction ${r.txid}. Start staking once it confirms.`,
      );
      el<HTMLFormElement>("stk-form").reset();
    } else if (r.outcome === "rejected") {
      stakeResult("warning", "Rejected. Nothing was sent.");
    } else if (r.outcome === "failed") {
      stakeResult("danger", `Not sent: ${r.error}`);
    }
    void refreshStake();
    void refreshWallet();
    void refreshRecent();
    return;
  }
  if (r.outcome === "sent") {
    sendResult("success", `Sent. Transaction ${r.txid}`);
    resetSendForm();
  } else if (r.outcome === "rejected") {
    sendResult("warning", "Rejected. Nothing was sent.");
  } else if (r.outcome === "failed") {
    sendResult("danger", `Not sent: ${r.error}`);
  }
  void refreshWallet();
  void refreshRecent();
  void refreshCoins();
  // A sent payment may have saved new labels.
  void refreshBook();
});
// The wallet an approval could not lock again; the Lock button locks that
// one, even if the page has switched to another wallet since.
let relockWallet: string | null = null;
await listen<string>("relock-failed", (event) => {
  relockWallet = event.payload;
  el("lock-warning-text").textContent = `Your last action went through, but ${
    relockWallet === "" ? "the default wallet" : `wallet ${relockWallet}`
  } may still be unlocked for a short while. Lock it now.`;
  el("lock-warning").hidden = false;
});
el("lock-warning-lock").addEventListener("click", async () => {
  try {
    await invoke("wallet_lock", { wallet: relockWallet });
    el("lock-warning").hidden = true;
    relockWallet = null;
  } catch (e) {
    el("lock-warning-text").textContent = `Still could not lock the wallet: ${String(e)}`;
  }
  void refreshWallets();
  void refreshWallet();
});
// Icon-only navigation (narrow windows) needs names that survive hiding the text.
for (const item of document.querySelectorAll<HTMLElement>(".nav-item")) {
  const label = item.querySelector(".nav-text")?.textContent?.trim();
  if (label) {
    item.setAttribute("aria-label", label);
    item.title = label;
  }
}
await listen<NodeStatus>("node-status", (event) => render(event.payload));
render(await invoke<NodeStatus>("node_status"));
