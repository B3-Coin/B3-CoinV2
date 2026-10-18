// The approval window. It shows only what the core decoded from the draft it
// holds, and its only powers are reading that draft and deciding on it.
import { invoke } from "@tauri-apps/api/core";
import { el, formatAmount, initTheme, sumAmounts } from "../shared/ui";

type OutputLine = { address: string; amount: string; change: boolean };
type DraftView = {
  id: string;
  summary: { chain: string; wallet: string; outputs: OutputLine[]; fee: string; inputs: number };
  // The wallet is locked: approving needs its passphrase.
  locked: boolean;
};

await initTheme();

function row(key: string, value: string, address?: string): HTMLElement {
  const drow = document.createElement("div");
  drow.className = "drow";
  const k = document.createElement("span");
  k.className = "dkey";
  k.textContent = key;
  if (address !== undefined) {
    const line = document.createElement("span");
    line.className = "addr-line";
    line.textContent = address;
    k.append(line);
  }
  const v = document.createElement("span");
  v.className = "dval amt";
  v.textContent = value;
  drow.append(k, v);
  return drow;
}

function show(draft: DraftView | null): void {
  el("empty").hidden = draft !== null;
  el("draft").hidden = draft === null;
  el("error").hidden = true;
  if (!draft) return;
  const s = draft.summary;
  const paid = s.outputs.filter((o) => !o.change);
  const sent = sumAmounts(paid.map((o) => o.amount));
  const total = sumAmounts([...paid.map((o) => o.amount), s.fee]);

  // The title restates what leaves the wallet, as b3_docker's confirmations do.
  el("title").textContent = sent === null ? "Approve this payment?" : `Send ${formatAmount(sent)}?`;
  el("from").textContent = `From wallet ${s.wallet} on ${s.chain}`;
  // Payments first, change last, so the money leaving reads first.
  const ordered = [...paid, ...s.outputs.filter((o) => o.change)];
  el("outputs").replaceChildren(
    ...ordered.map((o) =>
      row(o.change ? "Change back to this wallet" : "Pay", formatAmount(o.amount, 9), o.address),
    ),
  );
  el("fee").textContent = formatAmount(s.fee, 9);
  el("total").textContent = total === null ? "—" : formatAmount(total, 9);
  el("chain").textContent = s.chain;
  el("inputs").textContent = String(s.inputs);
  el("pass-field").hidden = !draft.locked;
  el<HTMLInputElement>("pass").value = "";
  if (draft.locked) el("pass").focus();
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
    const passphrase = approve && current.locked ? el<HTMLInputElement>("pass").value : null;
    el<HTMLInputElement>("pass").value = "";
    current = await invoke<DraftView | null>("approval_decide", { id: current.id, approve, passphrase });
    show(current);
  } catch (e) {
    el("error").hidden = false;
    el("error").textContent = String(e);
    el("error").scrollIntoView({ block: "nearest" });
    if (current?.locked) el("pass").focus();
  } finally {
    setBusy(false);
  }
}

el("approve").addEventListener("click", () => decide(true));
el("reject").addEventListener("click", () => decide(false));
current = await invoke<DraftView | null>("approval_pending");
show(current);
