import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import { amountNode, el, formatAmount, initTheme, shorten, toggleTheme, type Theme } from "../shared/ui";

// Mirrors NodeStatus in src-tauri/src/node.rs (serde tag "phase").
type NodeStatus =
  | { phase: "starting"; message: string }
  | { phase: "ready"; chain: string; blocks: number; best_block_hash: string }
  | { phase: "stopping" }
  | { phase: "failed"; error: string };

type WalletOverview = { wallet: string; spendable: string; immature: string; can_fund: boolean };

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
  home: { title: "Home", sub: "Your balance and your node" },
  send: { title: "Send", sub: "Pay someone in B3" },
  updates: { title: "Updates", sub: "Signed B3 Hive releases" },
} as const;
type View = keyof typeof VIEWS;

function go(view: View): void {
  for (const id of Object.keys(VIEWS)) el(`view-${id}`).hidden = id !== view;
  for (const item of document.querySelectorAll<HTMLElement>(".nav-item[data-view]")) {
    if (item.dataset.view === view) item.setAttribute("aria-current", "page");
    else item.removeAttribute("aria-current");
  }
  el("page-title").textContent = VIEWS[view].title;
  el("page-sub").textContent = VIEWS[view].sub;
  if (view === "send") el<HTMLInputElement>("address").focus();
  if (view === "updates") void refreshUpdates();
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
      break;
  }
}

// ---- wallet ----------------------------------------------------------------

async function refreshWallet(): Promise<void> {
  try {
    const w = await invoke<WalletOverview>("wallet_overview");
    el("wallet").hidden = false;
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
  const [text, tone] = STATE_TEXT[u.state] ?? [u.state, "neutral"];
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
  } else if (u.state === "idle" && u.offer === null && checked) {
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
});
await listen<NodeStatus>("node-status", (event) => render(event.payload));
render(await invoke<NodeStatus>("node_status"));
