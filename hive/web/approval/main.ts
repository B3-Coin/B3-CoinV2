// The approval window. It shows only what the core decoded from the draft it
// holds, and its only powers are reading that draft and deciding on it.
import { invoke } from "@tauri-apps/api/core";
import { el, formatAmount, initTheme, sumAmounts } from "../shared/ui";

type OutputLine = { address: string; amount: string; change: boolean };
type Summary = { chain: string; wallet: string; outputs: OutputLine[]; fee: string; inputs: number };
// Mirrors drafts::Request in src-tauri/src/drafts.rs.
type Request =
  | { kind: "pay"; summary: Summary }
  | { kind: "stake"; chain: string; wallet: string; amount: string; activation_depth: number }
  | { kind: "bind_finality"; chain: string; wallet: string; rotate: boolean };
type DraftView = {
  id: string;
  request: Request;
  // The wallet is locked: approving needs its passphrase.
  locked: boolean;
};

await initTheme();

function row(key: string, value: string, address?: string, amount = true): HTMLElement {
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
  v.className = amount ? "dval amt" : "dval";
  v.textContent = value;
  drow.append(k, v);
  return drow;
}

const walletName = (w: string): string => (w === "" ? "the default wallet" : `wallet ${w}`);

function showPay(s: Summary): void {
  const paid = s.outputs.filter((o) => !o.change);
  const sent = sumAmounts(paid.map((o) => o.amount));
  const total = sumAmounts([...paid.map((o) => o.amount), s.fee]);

  // The title restates what leaves the wallet, as b3_docker's confirmations do.
  el("title").textContent = sent === null ? "Approve this payment?" : `Send ${formatAmount(sent)}?`;
  el("from").textContent = `From ${walletName(s.wallet)} on ${s.chain}`;
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
  el("pass-help").textContent =
    "The wallet is locked. It unlocks only to sign this payment and locks again straight after.";
  el("approve").textContent = "Send now";
}

// Staking requests go to the node as they are; it picks the coins and the
// fee when they are approved, so the window says so instead of a figure.
function showAction(r: Exclude<Request, { kind: "pay" }>): void {
  el("from").textContent = `From ${walletName(r.wallet)} on ${r.chain}`;
  const fee = row("Network fee", "Set by the wallet when it is sent", undefined, false);
  if (r.kind === "stake") {
    el("title").textContent = `Stake ${formatAmount(r.amount)}?`;
    el("action-text").textContent =
      `The amount is locked as a stake for this wallet's validator. It stays in this wallet, but ordinary payments do not spend it. It counts for staking ${r.activation_depth} blocks after it confirms.`;
    el("action-rows").replaceChildren(row("Stake", formatAmount(r.amount, 9)), fee, row("Network", r.chain, undefined, false));
    el("approve").textContent = "Stake now";
  } else {
    el("title").textContent = r.rotate ? "Rotate the finality key?" : "Bind the finality key?";
    el("action-text").textContent = r.rotate
      ? "Sends a small transaction that replaces this validator's bound BLS finality key with a new one. It takes effect at the next epoch boundary."
      : "Sends a small transaction that registers this validator's BLS finality key. Staking needs it to sign finality. It takes effect at the next epoch boundary.";
    el("action-rows").replaceChildren(fee, row("Network", r.chain, undefined, false));
    el("approve").textContent = r.rotate ? "Rotate key" : "Bind key";
  }
  el("pass-help").textContent =
    "The wallet is locked. It unlocks only for this request and locks again straight after.";
}

function show(draft: DraftView | null): void {
  el("empty").hidden = draft !== null;
  el("draft").hidden = draft === null;
  el("error").hidden = true;
  if (!draft) return;
  const pay = draft.request.kind === "pay";
  el("pay-body").hidden = !pay;
  el("action-body").hidden = pay;
  if (draft.request.kind === "pay") showPay(draft.request.summary);
  else showAction(draft.request);
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
