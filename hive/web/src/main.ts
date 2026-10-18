import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import { amountNode, el, formatAmount, icon, initTheme, shorten, toggleTheme, type Theme } from "../shared/ui";

// Mirrors NodeStatus in src-tauri/src/node.rs (serde tag "phase").
type NodeStatus =
  | { phase: "starting"; message: string }
  | { phase: "ready"; chain: string; blocks: number; best_block_hash: string }
  | { phase: "stopping" }
  | { phase: "failed"; error: string }
  | { phase: "setup" };

type WalletOverview = { wallet: string; spendable: string; immature: string; can_fund: boolean };

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
  activity: { title: "Activity", sub: "Your wallet's transactions" },
  network: { title: "Network", sub: "Your node and its peers" },
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
  if (view === "network") void refreshNetwork();
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

async function refreshWallet(): Promise<void> {
  try {
    const w = await invoke<WalletOverview | null>("wallet_overview");
    el("no-wallet").hidden = w !== null;
    el("wallet").hidden = w === null;
    if (!w) return;
    el("wallet-name").textContent = `Wallet ${w.wallet}`;
    el("spendable").replaceChildren(amountNode(w.spendable, "amt-hero"));
    const maturing = !/^0*\.?0*$/.test(w.immature);
    el("immature-chip").hidden = !maturing;
    el("immature").textContent = `${formatAmount(w.immature)} maturing`;
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

const ACTIVITY_STEP = 100;
const ACTIVITY_LIMIT = 500; // wallet::ACTIVITY_LIMIT
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

function activityResult(text: string): void {
  const box = el("activity-result");
  box.hidden = !text;
  box.className = "banner banner-danger mt-4";
  box.textContent = text;
}

const refreshActivity = single(async () => {
  try {
    const lines = await invoke<ActivityEntry[]>("wallet_activity", { count: activityShown });
    activityResult("");
    el("activity-list").replaceChildren(...activityList(lines));
    el("activity-more-row").hidden = !(lines.length === activityShown && activityShown < ACTIVITY_LIMIT);
    el("activity-count").textContent = lines.length
      ? `Newest ${plural(lines.length, "line")}${lines.length === ACTIVITY_LIMIT ? ", the most shown" : ""}`
      : "";
  } catch (e) {
    activityResult(`The wallet's transactions could not be read: ${String(e)}`);
  }
});

el("activity-more").addEventListener("click", () => {
  activityShown = Math.min(ACTIVITY_LIMIT, activityShown + ACTIVITY_STEP);
  void refreshActivity();
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

function peerRow(p: Peer): HTMLTableRowElement {
  const tr = document.createElement("tr");
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

const refreshNetwork = single(async () => {
  try {
    const [overview, peers] = await Promise.all([
      invoke<ChainOverview>("chain_overview"),
      invoke<Peer[]>("network_peers"),
    ]);
    networkResult("");
    renderNodeInfo(overview);
    el("peer-count").textContent = peers.length ? plural(peers.length, "peer") : "";
    el("peer-table").hidden = peers.length === 0;
    el("peer-empty").hidden = peers.length > 0;
    el("peer-rows").replaceChildren(...peers.map(peerRow));
  } catch (e) {
    networkResult(`The node's network state could not be read: ${String(e)}`);
  }
});

// The node's status changes only with a new block; the sync figures and the
// peer list move in between.
const REFRESH_MS = 5000;
setInterval(() => {
  if (!wasReady || document.hidden) return;
  if (current === "home") void refreshOverview();
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
