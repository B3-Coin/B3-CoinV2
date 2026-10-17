// The approval window. It shows only what the core decoded from the draft it
// holds, and its only powers are reading that draft and deciding on it.
import { invoke } from "@tauri-apps/api/core";

type OutputLine = { address: string; amount: string; change: boolean };
type DraftView = {
  id: string;
  summary: { chain: string; wallet: string; txid: string; outputs: OutputLine[]; fee: string };
};

function el<T extends HTMLElement>(id: string): T {
  const node = document.getElementById(id);
  if (!node) throw new Error(`missing #${id}`);
  return node as T;
}

function show(draft: DraftView | null): void {
  el("empty").hidden = draft !== null;
  el("draft").hidden = draft === null;
  if (!draft) return;
  const s = draft.summary;
  el("chain").textContent = s.chain;
  el("wallet").textContent = s.wallet;
  el("fee").textContent = `${s.fee} B3`;
  el("txid").textContent = s.txid;
  const list = el("outputs");
  list.replaceChildren(
    ...s.outputs.map((o) => {
      const item = document.createElement("li");
      item.textContent = `${o.amount} B3 to ${o.address}${o.change ? " (change back to this wallet)" : ""}`;
      return item;
    }),
  );
}

let current: DraftView | null = null;

function setBusy(busy: boolean): void {
  el<HTMLButtonElement>("approve").disabled = busy;
  el<HTMLButtonElement>("reject").disabled = busy;
}

// After a decision the core returns the next waiting draft, if any, and
// closes this window itself when nothing is left.
async function decide(approve: boolean): Promise<void> {
  if (!current) return;
  setBusy(true);
  try {
    current = await invoke<DraftView | null>("approval_decide", { id: current.id, approve });
    show(current);
  } catch (e) {
    el("error").textContent = String(e);
  } finally {
    setBusy(false);
  }
}

el("approve").addEventListener("click", () => decide(true));
el("reject").addEventListener("click", () => decide(false));
current = await invoke<DraftView | null>("approval_pending");
show(current);
