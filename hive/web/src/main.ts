import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import { amountNode, el, formatAmount, icon, initTheme, setMasked, shorten, toggleTheme, type Theme } from "../shared/ui";

// Mirrors NodeStatus in src-tauri/src/node.rs (serde tag "phase").
type NodeStatus =
  | { phase: "starting"; message: string }
  | { phase: "ready"; chain: string; blocks: number; best_block_hash: string }
  | { phase: "stopping" }
  | { phase: "failed"; error: string }
  | { phase: "setup" };

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
  | { outcome: "rejected"; id: string }
  | { outcome: "failed"; id: string; error: string };

function showThemeIcon(theme: Theme): void {
  // The button shows the theme it switches to.
  el("theme-icon").setAttribute("href", `/icons.svg#i-${theme === "dark" ? "sun" : "moon"}`);
}
showThemeIcon(await initTheme());

// ---- views -----------------------------------------------------------------

const VIEWS = {
  setup: { title: "Set up B3 Hive", sub: "Get the B3 chain for your node" },
  home: { title: "Home", sub: "Your balance and your node" },
  send: { title: "Send", sub: "Pay someone in B3" },
  receive: { title: "Receive", sub: "Addresses and payment links for this wallet" },
  activity: { title: "Activity", sub: "Your wallet's transactions" },
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
  if (view === "send") el<HTMLInputElement>("address").focus();
  if (view === "updates") void refreshUpdates();
  if (view === "activity") void refreshActivity();
  if (view === "network") {
    void refreshNetwork();
    drawTraffic();
  }
  if (view === "settings") void loadSettings();
  if (view === "wallets") void refreshWallets();
  if (view === "receive") void refreshReceiving();
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
  }
});

// ---- node status -----------------------------------------------------------

type Tone = "info" | "danger";

function showBanner(tone: Tone, title: string, detail: string): void {
  const banner = el("node-banner");
  banner.hidden = false;
  banner.className = `banner banner-${tone}`;
  el("node-banner-spinner").hidden = tone !== "info";
  el("node-banner-icon").hidden = tone === "info";
  el("node-banner-title").textContent = title;
  el("node-banner-detail").textContent = detail;
  el("restart-node").hidden = true;
}

function badge(state: "unlocked" | "locked" | "no-node" | "no-wallet", text: string): void {
  el("node-badge").dataset.state = state;
  el("node-badge-text").textContent = text;
}

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
    case "starting":
      showBanner("info", "Starting the node", status.message);
      badge("no-wallet", "Starting");
      break;
    case "ready":
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
      showBanner("info", "Stopping the node", "");
      badge("locked", "Stopping");
      break;
    case "failed":
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
    badgeEl.textContent = mine ? "Running" : s.running ? "Other wallet" : "Stopped";
    badgeEl.className = `badge badge-${mine ? "success" : "neutral"}`;
    if (mine) {
      lines.push("Staking is running.");
      if (s.state) lines.push(s.state);
    } else if (s.running) {
      lines.push("Another wallet is staking on this node.");
    } else {
      lines.push("Staking is stopped.");
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

async function refreshWallet(): Promise<void> {
  try {
    const w = await invoke<WalletOverview | null>("wallet_overview");
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
    el("fund").hidden = !w.can_fund;
  } catch (e) {
    showBanner("danger", "The wallet could not be read", String(e));
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
type Receiving = { address: string; label: string; received: string; transactions: number };

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

function showRequest(r: ReceiveRequest, title: string): void {
  el("rcv-request").hidden = false;
  el("rcv-request-title").textContent = title;
  el("rcv-qr").hidden = r.qr === null;
  el("rcv-qr-note").hidden = r.qr !== null;
  if (r.qr) drawQr(el("rcv-qr"), r.qr);
  const rows = [detailRow("Address", r.address, r.address)];
  if (r.label) rows.push(detailRow("Label", r.label));
  // The requested amount is shown exactly, never masked: it is what the
  // payer will be asked for.
  if (r.amount !== null) rows.push(detailRow("Amount", `${r.amount} B3`));
  if (r.message) rows.push(detailRow("Message", r.message));
  rows.push(detailRow("Payment link", r.uri, r.uri));
  el("rcv-rows").replaceChildren(...rows);
  el("rcv-request").scrollIntoView({ block: "nearest" });
}

const refreshReceiving = single(async () => {
  try {
    const list = await invoke<Receiving[]>("receive_addresses");
    // Newest first, as Qt's history sorts.
    list.reverse();
    el("rcv-count").textContent = list.length ? plural(list.length, "address", "addresses") : "";
    el("rcv-empty").hidden = list.length > 0;
    el("rcv-list").replaceChildren(
      ...list.map((a) => {
        const row = document.createElement("button");
        row.type = "button";
        row.className = "list-row clickable";
        row.title = "Show this address's link and QR code";
        const lead = document.createElement("div");
        lead.className = "lead in";
        lead.append(icon("arrow-down"));
        const body = document.createElement("div");
        body.className = "body";
        const title = document.createElement("span");
        title.textContent = a.label || "(no label)";
        const meta = document.createElement("span");
        meta.className = "meta is-mono";
        meta.textContent = shorten(a.address, 14, 8);
        meta.title = a.address;
        body.append(title, meta);
        const trail = document.createElement("div");
        trail.className = "trail";
        const got = document.createElement("span");
        got.className = "t-sm muted";
        got.textContent = a.transactions ? plural(a.transactions, "payment") : "Nothing received";
        trail.append(a.transactions ? amountNode(a.received) : document.createElement("span"), got);
        row.append(lead, body, trail);
        row.addEventListener("click", async () => {
          try {
            showRequest(await invoke<ReceiveRequest>("receive_show", { address: a.address }), a.label || "Receiving address");
          } catch (e) {
            receiveResult(String(e));
          }
        });
        return row;
      }),
    );
  } catch (e) {
    receiveResult(String(e));
  }
});

function receiveResult(text: string): void {
  el("rcv-result").hidden = !text;
  el("rcv-result").textContent = text;
}

el("rcv-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  const button = el<HTMLButtonElement>("rcv-submit");
  button.disabled = true;
  receiveResult("");
  try {
    const r = await invoke<ReceiveRequest>("receive_new", {
      label: el<HTMLInputElement>("rcv-label").value,
      amount: el<HTMLInputElement>("rcv-amount").value,
      message: el<HTMLInputElement>("rcv-message").value,
    });
    for (const id of ["rcv-label", "rcv-amount", "rcv-message"]) el<HTMLInputElement>(id).value = "";
    showRequest(r, "Request payment");
    void refreshReceiving();
  } catch (err) {
    receiveResult(String(err));
  } finally {
    button.disabled = false;
  }
});

// ---- wallets -----------------------------------------------------------------
// Qt's wallet selector and File menu. The default wallet's name is "".

// Mirrors WalletsView in src-tauri/src/lib.rs.
type WalletsView = { loaded: string[]; available: string[]; current: string | null };
type WalletAction = { action: "switch" | "open" | "close" | "create"; name: string };

const walletName = (name: string) => (name === "" ? "Default wallet" : name);

function walletsResult(tone: "success" | "danger" | "", text: string): void {
  const box = el("wl-result");
  box.hidden = !text;
  box.className = `banner banner-${tone}`;
  box.textContent = text;
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
      // Two clicks, as Qt asks before closing a wallet.
      trail.push(
        actionButton("Close", "btn-ghost", (b) => {
          if (b.dataset.armed) void manageWallet({ action: "close", name });
          else {
            b.dataset.armed = "1";
            b.textContent = "Confirm close";
            b.className = "btn btn-sm btn-danger-quiet";
          }
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
  el("wl-backup-name").textContent = v.current === null ? "the wallet in use" : walletName(v.current);
  el<HTMLButtonElement>("wl-backup").disabled = v.current === null;
  renderWalletPick(v);
}

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

async function manageWallet(action: WalletAction): Promise<void> {
  if (walletBusy) return;
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
    // Every page shows the wallet in use.
    void refreshWallet();
    void refreshRecent();
    el("rcv-request").hidden = true;
    if (current === "receive") void refreshReceiving();
  } catch (e) {
    walletsResult("danger", String(e));
    void refreshWallets();
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
  await manageWallet({ action: "create", name: input.value });
  if (el("wl-result").classList.contains("banner-success")) input.value = "";
  button.disabled = false;
});

el("wl-backup").addEventListener("click", async () => {
  const button = el<HTMLButtonElement>("wl-backup");
  button.disabled = true;
  try {
    const path = await invoke<string>("wallet_backup");
    walletsResult("success", `Saved the backup to ${path}`);
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
function single(refresh: () => Promise<void>): () => Promise<void> {
  let running = false;
  return async () => {
    if (running) return;
    running = true;
    try {
      await refresh();
    } finally {
      running = false;
    }
  };
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
    fee.textContent = `Fee ${formatAmount(e.fee.replace(/^-/, ""))}`;
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

const refreshRecent = single(async () => {
  try {
    const lines = await invoke<ActivityEntry[]>("wallet_activity", { count: 5 });
    el("recent").hidden = false;
    el("recent-list").replaceChildren(...activityList(lines));
  } catch {
    el("recent").hidden = true;
  }
});

function activityResult(tone: "danger" | "success", text: string): void {
  const box = el("activity-result");
  box.hidden = !text;
  box.className = `banner banner-${tone} mt-4`;
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

const refreshActivity = single(async () => {
  try {
    const page = await invoke<HistoryPage>("wallet_history", { filter: historyFilter(), count: activityShown });
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
    activityResult("danger", `The wallet's transactions could not be read: ${String(e)}`);
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
    const path = await invoke<string>("wallet_export_csv", { filter: historyFilter() });
    activityResult("success", `Saved the transactions shown by these filters to ${path}`);
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

/// The details dialog shared by transactions and peers.
function openDetails(title: string, sub: string, rows: HTMLElement[]): void {
  detailReturnFocus = document.activeElement as HTMLElement | null;
  el("detail-title").textContent = title;
  el("detail-sub").textContent = sub;
  el("detail-rows").replaceChildren(...rows);
  el("detail-backdrop").hidden = false;
  el("detail-close").focus();
}

function closeDetails(): void {
  el("detail-backdrop").hidden = true;
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
    if (d.fee) rows.push(detailRow("Fee", formatAmount(d.fee.replace(/^-/, ""))));
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
  tr.tabIndex = 0;
  tr.title = "Show this peer's details";
  tr.addEventListener("click", () => showPeer(p));
  tr.addEventListener("keydown", (k) => {
    if (k.key === "Enter" || k.key === " ") {
      k.preventDefault();
      showPeer(p);
    }
  });
  const direction = DIRECTION[p.connection_type] ?? (p.inbound ? "Inbound" : "Outbound");
  tr.append(
    cell(p.address, "mono"),
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
  box.className = "banner banner-danger";
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
  box.className = `banner banner-${tone}`;
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
  box.className = `banner banner-${tone}`;
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
  for (const id of ["set-dbcache", "set-par", "set-proxy", "set-onion"]) el(id).removeAttribute("aria-invalid");
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

/// An optional whole number from a field, or an error naming the field.
function wholeNumber(id: string, name: string): number | null {
  const input = el<HTMLInputElement>(id);
  const text = input.value.trim();
  input.removeAttribute("aria-invalid");
  if (text === "") return null;
  if (!/^-?\d+$/.test(text)) {
    input.setAttribute("aria-invalid", "true");
    throw new Error(`${name} must be a whole number.`);
  }
  return Number(text);
}

function proxyField(useId: string, id: string, name: string): string | null {
  const input = el<HTMLInputElement>(id);
  input.removeAttribute("aria-invalid");
  if (!el<HTMLInputElement>(useId).checked) return null;
  const text = input.value.trim();
  if (!text) {
    input.setAttribute("aria-invalid", "true");
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

el("settings-form").addEventListener("submit", (event) => {
  event.preventDefault();
  let settings: NodeSettings;
  try {
    settings = readSettingsForm();
  } catch (e) {
    settingsResult("danger", e instanceof Error ? e.message : String(e));
    return;
  }
  void applySettings(settings);
});

el("settings-defaults").addEventListener("click", () => {
  void applySettings({
    dbcache_mib: null,
    script_threads: null,
    listen: null,
    natpmp: null,
    proxy: null,
    onion: null,
  });
});

// ---- send ------------------------------------------------------------------

function sendResult(tone: "info" | "success" | "warning" | "danger", text: string): void {
  const box = el("send-result");
  box.hidden = false;
  box.className = `banner banner-${tone} mt-4`;
  box.textContent = text;
}

// The page only asks for a draft. The core builds and signs it, and nothing
// is sent unless the approval window approves it.
el("send").addEventListener("submit", async (event) => {
  event.preventDefault();
  const submit = el<HTMLButtonElement>("send-submit");
  submit.disabled = true;
  sendResult("info", "Preparing the payment…");
  try {
    await invoke<string>("draft_send", {
      address: el<HTMLInputElement>("address").value.trim(),
      amount: el<HTMLInputElement>("amount").value.trim(),
    });
    sendResult("info", "Waiting for your decision in the approval window.");
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
  box.className = `banner banner-${tone} mt-4`;
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

await listen<DraftResult>("draft-result", (event) => {
  const r = event.payload;
  if (r.outcome === "sent") {
    sendResult("success", `Sent. Transaction ${r.txid}`);
    el<HTMLFormElement>("send").reset();
  } else if (r.outcome === "rejected") {
    sendResult("warning", "Rejected. Nothing was sent.");
  } else {
    sendResult("danger", `Not sent: ${r.error}`);
  }
  void refreshWallet();
  void refreshRecent();
});
await listen<NodeStatus>("node-status", (event) => render(event.payload));
render(await invoke<NodeStatus>("node_status"));
