import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";

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

function el<T extends HTMLElement>(id: string): T {
  const node = document.getElementById(id);
  if (!node) throw new Error(`missing #${id}`);
  return node as T;
}

// Text only: node replies can carry attacker-chosen strings, so nothing here
// ever goes through innerHTML.
function render(status: NodeStatus): void {
  const chain = el("chain");
  chain.hidden = status.phase !== "ready";
  switch (status.phase) {
    case "starting":
      el("phase").textContent = "Starting the node…";
      el("detail").textContent = status.message;
      break;
    case "ready":
      el("phase").textContent = "The node is running.";
      el("detail").textContent = "";
      el("chain-name").textContent = status.chain;
      el("chain-blocks").textContent = String(status.blocks);
      el("chain-best").textContent = status.best_block_hash;
      void refreshWallet();
      break;
    case "stopping":
      el("phase").textContent = "Stopping the node…";
      el("detail").textContent = "";
      break;
    case "failed":
      el("phase").textContent = "The node could not start.";
      el("detail").textContent = status.error;
      break;
  }
}

async function refreshWallet(): Promise<void> {
  try {
    const w = await invoke<WalletOverview>("wallet_overview");
    el("wallet").hidden = false;
    el("wallet-name").textContent = w.wallet;
    el("spendable").textContent = w.spendable;
    el("immature").textContent = w.immature;
    el("fund").hidden = !w.can_fund;
  } catch (e) {
    el("send-result").textContent = String(e);
  }
}

el("open-console").addEventListener("click", () => {
  invoke("open_console").catch((e) => (el("detail").textContent = String(e)));
});

el("fund").addEventListener("click", async () => {
  const button = el<HTMLButtonElement>("fund");
  button.disabled = true;
  try {
    await invoke("dev_fund");
    await refreshWallet();
  } catch (e) {
    el("send-result").textContent = String(e);
  } finally {
    button.disabled = false;
  }
});

// The page only asks for a draft. The core builds and signs it, and nothing
// is sent unless the approval window approves it.
el("send").addEventListener("submit", async (event) => {
  event.preventDefault();
  el("send-result").textContent = "Preparing the transaction…";
  try {
    await invoke<string>("draft_send", {
      address: el<HTMLInputElement>("address").value,
      amount: el<HTMLInputElement>("amount").value.trim(),
    });
    el("send-result").textContent = "Waiting for your decision in the approval window.";
  } catch (e) {
    el("send-result").textContent = String(e);
  }
});

await listen<DraftResult>("draft-result", (event) => {
  const r = event.payload;
  el("send-result").textContent =
    r.outcome === "sent" ? `Sent. Transaction ${r.txid}` : r.outcome === "rejected" ? "Rejected. Nothing was sent." : `Not sent: ${r.error}`;
  void refreshWallet();
});
await listen<NodeStatus>("node-status", (event) => render(event.payload));
render(await invoke<NodeStatus>("node_status"));
