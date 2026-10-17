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
}

for (const button of document.querySelectorAll<HTMLElement>("[data-view]")) {
  button.addEventListener("click", () => go(button.dataset.view as View));
}

el("open-console").addEventListener("click", () => {
  invoke("open_console").catch((e) => showBanner("danger", "The console did not open", String(e)));
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
}

function badge(state: "unlocked" | "locked" | "no-node" | "no-wallet", text: string): void {
  el("node-badge").dataset.state = state;
  el("node-badge-text").textContent = text;
}

function render(status: NodeStatus): void {
  const ready = status.phase === "ready";
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
      showBanner("danger", "The node could not start", status.error);
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
