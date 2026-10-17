import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";

// Mirrors NodeStatus in src-tauri/src/node.rs (serde tag "phase").
type NodeStatus =
  | { phase: "starting"; message: string }
  | { phase: "ready"; chain: string; blocks: number; best_block_hash: string }
  | { phase: "stopping" }
  | { phase: "failed"; error: string };

function el(id: string): HTMLElement {
  const node = document.getElementById(id);
  if (!node) throw new Error(`missing #${id}`);
  return node;
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

await listen<NodeStatus>("node-status", (event) => render(event.payload));
render(await invoke<NodeStatus>("node_status"));
