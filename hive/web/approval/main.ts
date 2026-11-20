// The approval window. It shows only what the core decoded from the draft it
// holds, and its only powers are reading that draft and deciding on it.
import { invoke } from "@tauri-apps/api/core";
import { count, el, formatAmount, initTheme, sumAmounts } from "../shared/ui";
import { amountText, feeRate, lotText, priceText, sellerFee, shortId, tickText, tifText, unit, whole, type Asset, type Market } from "../src/fm2fmt";
import { LONG_WAIT_SECONDS, shareText, stakeOutlook, waitText } from "../src/outlook";

type OutputLine = { address: string; amount: string; change: boolean; mine: boolean };
type Summary = { chain: string; wallet: string; outputs: OutputLine[]; fee: string; inputs: number };
// Mirrors psbt::Summary: a PSBT made elsewhere, so its inputs need not be
// this wallet's, and its fee is unknown when it lacks the input amounts.
type PsbtSummary = {
  chain: string;
  wallet: string;
  outputs: OutputLine[];
  fee: string | null;
  inputs: number;
  finalized: number;
  complete: boolean;
};
// Mirrors fm2::view::ActionDraft and Intent: amounts are exact integer
// strings of atoms, prices of ticks and sizes of lots.
type Fm2Amount = { asset: Asset; atoms: string };
type Fm2Intent =
  | { action: "place"; market: Market; side: "buy" | "sell"; tif: string; price_ticks: string; lots: string; base: Fm2Amount; quote: Fm2Amount }
  | { action: "cancel"; order: string; placed: Fm2Intent | null }
  | { action: "cancel_all" }
  | { action: "transfer"; to: string; amount: Fm2Amount }
  | { action: "other"; kind: number };
type ActionDraft = {
  chain: string;
  domain: string;
  account: string;
  intent: Fm2Intent;
  window_slots: number;
  seats: number;
  fee_ppm: number;
};
// Mirrors drafts::Request in src-tauri/src/drafts.rs.
type Request =
  | { kind: "pay"; summary: Summary }
  | { kind: "release"; summary: Summary; producers: Producers }
  | { kind: "sign_psbt"; summary: PsbtSummary }
  | { kind: "send_psbt"; summary: PsbtSummary }
  | {
      kind: "stake";
      chain: string;
      wallet: string;
      amount: string;
      activation_depth: number;
      producers: Producers;
      active_weight: number;
      total_active_weight: number;
    }
  | { kind: "bind_finality"; chain: string; wallet: string; rotate: boolean }
  | { kind: "revoke_finality"; chain: string; wallet: string }
  | { kind: "fm2"; action: ActionDraft };
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

/** A row with a line under its label saying what the term means. */
function explained(key: string, value: string, hint: string): HTMLElement {
  const r = row(key, value, undefined, false);
  const line = document.createElement("span");
  line.className = "dhint";
  line.textContent = hint;
  r.querySelector(".dkey")?.append(line);
  return r;
}

const walletName = (w: string): string => (w === "" ? "the default wallet" : `wallet ${w}`);

function showPay(s: Summary): void {
  const paid = s.outputs.filter((o) => !o.change);
  // Payments to this wallet's own addresses stay in it.
  const away = paid.filter((o) => !o.mine);
  const sent = sumAmounts(away.map((o) => o.amount));
  const kept = sumAmounts(paid.filter((o) => o.mine).map((o) => o.amount));
  const total = sumAmounts([...away.map((o) => o.amount), s.fee]);

  // The title restates what leaves the wallet, as b3_docker's confirmations do.
  el("title").textContent =
    sent === null || kept === null
      ? "Approve this payment?"
      : away.length === 0
        ? `Move ${formatAmount(kept)} within this wallet?`
        : `Send ${formatAmount(sent)}?`;
  el("from").textContent = `From ${walletName(s.wallet)} on ${s.chain}`;
  // Payments first, change last, so the money leaving reads first.
  const ordered = [...paid, ...s.outputs.filter((o) => o.change)];
  el("outputs").replaceChildren(
    ...ordered.map((o) =>
      row(o.change ? "Change back to this wallet" : o.mine ? "To this wallet" : "Pay", formatAmount(o.amount, 9), o.address),
    ),
  );
  el("fee").textContent = formatAmount(s.fee, 9);
  el("total").textContent = total === null ? "—" : formatAmount(total, 9);
  el("chain").textContent = s.chain;
  el("inputs").textContent = String(s.inputs);
  el("total-row").hidden = false;
  el("signed-row").hidden = true;
  el("pay-text").textContent = "This payment is permanent once sent. Check every line before you approve it.";
  el("pass-help").textContent =
    "The wallet is locked. It unlocks only to sign this payment and locks again straight after.";
  el("approve").textContent = "Send now";
}

// Mirrors stake::Producers: whether stake changes who makes blocks soon.
type Producers = { frozen: boolean; stake_enters_at: number | null };
const FROZEN_SET = "The network's set of block producers is frozen until an upcoming network upgrade";

// A release spends only stakes, to one address of this wallet; what was
// staked is what comes back plus the fee.
function showRelease(s: Summary, producers: Producers): void {
  showPay(s);
  const principal = sumAmounts([...s.outputs.map((o) => o.amount), s.fee]);
  el("title").textContent =
    principal === null ? "Release this stake?" : `Release ${formatAmount(principal)} from staking?`;
  const what = s.inputs === 1 ? "This stake stops" : `These ${s.inputs} stakes stop`;
  el("pay-text").textContent = producers.frozen
    ? `${what} counting for your validator once the release confirms. ${FROZEN_SET}: until then your validator keeps its place and weight in it, released or not. The B3 comes back to this wallet as ordinary coins, less the network fee.`
    : `${what} counting for your validator once the release confirms; weight already in the current epoch snapshot can count until it is replaced. The B3 comes back to this wallet as ordinary coins, less the network fee.`;
  el("pass-help").textContent =
    "The wallet is locked. It unlocks only to sign this release and locks again straight after.";
  el("approve").textContent = "Release now";
}

// A PSBT from elsewhere: its outputs and fee are shown as they are, without
// a total leaving this wallet, since its inputs may belong to others.
function showPsbt(kind: "sign_psbt" | "send_psbt", s: PsbtSummary): void {
  el("from").textContent = `With ${walletName(s.wallet)} on ${s.chain}`;
  el("outputs").replaceChildren(
    ...s.outputs.map((o) => row(o.mine ? "To this wallet" : "Pay", formatAmount(o.amount, 9), o.address)),
  );
  el("fee").textContent = s.fee === null ? "Not in the PSBT" : formatAmount(s.fee, 9);
  el("total-row").hidden = true;
  el("chain").textContent = s.chain;
  el("inputs").textContent = String(s.inputs);
  el("signed-row").hidden = false;
  el("signed").textContent = `${s.finalized} of ${s.inputs}`;
  if (kind === "sign_psbt") {
    el("title").textContent = "Sign this PSBT?";
    el("pay-text").textContent =
      "This wallet signs the inputs it holds keys for. Nothing is sent: the signed PSBT goes back to the Send page, to pass on or to broadcast. Check the outputs before you sign.";
    el("pass-help").textContent =
      "The wallet is locked. It unlocks only to sign this PSBT and locks again straight after.";
    el("approve").textContent = "Sign";
  } else {
    el("title").textContent = "Broadcast this transaction?";
    el("pay-text").textContent =
      "Every input is signed. Broadcasting sends it to the network, and it is permanent once sent. Check every line before you approve it.";
    el("approve").textContent = "Broadcast now";
  }
}

// Staking requests go to the node as they are; it picks the coins and the
// fee when they are approved, so the window gives the bound the node is
// started with (-maxtxfee) instead of a figure.
const FEE_BOUND = "At most 0.1 B3";
const FEE_LATER = "The node picks the network fee when it builds the transaction, at most 0.1 B3 and usually far less; Activity shows it once sent.";

function showAction(r: Extract<Request, { kind: "stake" | "bind_finality" | "revoke_finality" }>): void {
  el("from").textContent = `From ${walletName(r.wallet)} on ${r.chain}`;
  const fee = row("Network fee", FEE_BOUND, undefined, false);
  if (r.kind === "stake") {
    el("title").textContent = `Stake ${formatAmount(r.amount)}?`;
    const enters = r.producers.stake_enters_at;
    // What the stake would earn, so a small one is locked knowingly: with
    // it, the validator holds a share of the counting stake and makes about
    // that share of the blocks.
    const o = stakeOutlook(r.amount, r.active_weight, r.total_active_weight);
    const small = o !== null && o.secondsBetween >= LONG_WAIT_SECONDS;
    el("action-text").textContent =
      `The amount is locked as a stake for this wallet's validator. It stays in this wallet, but ordinary payments do not spend it. It becomes active ${r.activation_depth} blocks after it confirms` +
      (enters !== null
        ? `, and makes blocks from block ${count(enters)} at the earliest.`
        : r.producers.frozen
          ? `. ${FROZEN_SET}, so it makes blocks only after that.`
          : ", and makes blocks once the validator set next rotates to include it.") +
      (o
        ? ` With it your validator would make ${shareText(o)} of the network's blocks, one every ${waitText(o.secondsBetween).replace(/^about /, "")} on average, if the counting stake stays as it is.`
        : "") +
      (small
        ? " That is a small stake: it mostly supports the network. A validator that goes offline for long should revoke its finality key first, on the Stake page, so its weight does not hold the finality votes back while it cannot vote."
        : "") +
      ` ${FEE_LATER}`;
    el("action-rows").replaceChildren(
      row("Stake", formatAmount(r.amount, 9)),
      ...(o
        ? [
            explained("Share of blocks", shareText(o), "Estimated from your share of the counting stake; it changes as stakes come and go."),
            explained("One block every", waitText(o.secondsBetween).replace(/^about /, ""), "On average, at one block a minute, while your node is online."),
          ]
        : []),
      fee,
      row("Network", r.chain, undefined, false),
    );
    el("approve").textContent = "Stake now";
  } else if (r.kind === "revoke_finality") {
    el("title").textContent = "Revoke the finality key?";
    el("action-text").textContent =
      `Sends a small transaction that withdraws this validator's bound BLS finality key. From the next epoch boundary your validator is out of the validator set: it makes no blocks, and its weight no longer counts toward the votes that make blocks final, so the other validators are not held back while yours is offline. Your stakes stay as they are. Bind a new key when you are back; staking needs one. ${FEE_LATER}`;
    el("action-rows").replaceChildren(fee, row("Network", r.chain, undefined, false));
    el("approve").textContent = "Revoke key";
  } else {
    el("title").textContent = r.rotate ? "Rotate the finality key?" : "Bind the finality key?";
    el("action-text").textContent = r.rotate
      ? "Sends a small transaction that replaces this validator's bound BLS finality key with a new one. It takes effect at the next epoch boundary."
      : "Sends a small transaction that registers this validator's BLS finality key. Staking needs it to sign finality. It takes effect at the next epoch boundary.";
    el("action-text").textContent += ` ${FEE_LATER}`;
    el("action-rows").replaceChildren(fee, row("Network", r.chain, undefined, false));
    el("approve").textContent = r.rotate ? "Rotate key" : "Bind key";
  }
  el("pass-help").textContent =
    "The wallet is locked. It unlocks only for this request and locks again straight after.";
}

// A FlowMesh V2 action, in the words and units of the Trade page: the same
// functions format both, so a price reads the same on the page, here and
// in the result. No wallet signs it and no B3 moves on the chain: the
// network's own account key signs, and the validators decide. What a first
// look needs comes first; the exact integers that are signed, and where it
// goes, are under Details.
type Fm2Place = Extract<Fm2Intent, { action: "place" }>;

const fm2Size = (i: Fm2Place): string => amountText(BigInt(i.base.atoms), i.base.asset);

function placeLine(i: Fm2Place): string {
  return `${i.side === "buy" ? "Buy" : "Sell"} ${fm2Size(i)} at ${priceText(BigInt(i.price_ticks), i.market)}`;
}

function showFm2(a: ActionDraft): void {
  const i = a.intent;
  el("from").textContent = "On a FlowMesh test network, with test funds";
  const more: HTMLElement[] = [
    row("Trading account", shortId(a.account, 10, 6), a.account, false),
    explained("Valid for", `about ${a.window_slots} seconds`, `${a.window_slots} slots of the network. If it is not confirmed by then, it never can be.`),
    explained("Sent to", `${a.seats} validators`, "The same signed action goes to every validator of the network."),
    row("Network", a.chain, a.domain, false),
  ];
  const signed = "Approving signs it with your trading account's key and sends it to the network. The Trade page shows the result once the network confirms it.";
  let rows: HTMLElement[] = [];
  if (i.action === "place") {
    const buy = i.side === "buy";
    const m = i.market;
    const ticks = BigInt(i.price_ticks);
    const quote = BigInt(i.quote.atoms);
    const fee = sellerFee(quote, a.fee_ppm);
    const limit = priceText(ticks, m);
    const base = unit(m.base);
    const atOnce = i.tif === "ioc";
    el("title").textContent = atOnce ? `${buy ? "Buy" : "Sell"} ${fm2Size(i)}?` : `${placeLine(i)}?`;
    const how = atOnce
      ? buy
        ? `It trades at once with the sellers waiting in the order book, at their prices, the best first, and never above ${limit}. Whatever cannot trade at once is dropped, and what it would have cost stays yours.`
        : `It trades at once with the buyers waiting in the order book, at their prices, the best first, and never below ${limit}. Whatever cannot trade at once is dropped and stays yours.`
      : i.tif === "gtc"
        ? buy
          ? "It trades at once with anyone selling at this price or lower. The rest waits in the order book until someone sells at your price, or you cancel it. While it waits, the most it can cost is set aside from your balance."
          : `It trades at once with anyone buying at this price or higher. The rest waits in the order book until someone buys at your price, or you cancel it. While it waits, the ${base} it sells is set aside from your balance.`
        : `It only ever waits in the order book, until someone ${buy ? "sells" : "buys"} at your price or you cancel it. If it could trade at once, the network refuses it and nothing happens. While it waits, ${buy ? "the most it can cost" : `the ${base} it sells`} is set aside from your balance.`;
    const fees = buy ? "Buyers pay no fee." : `The seller pays a fee of ${feeRate(a.fee_ppm)} of what the sale brings, taken from it.`;
    el("action-text").textContent = `${how} ${fees} ${signed}`;
    rows = [
      atOnce
        ? explained(buy ? "You buy" : "You sell", fm2Size(i), `Less if the ${buy ? "offers" : "buyers"} are gone by the time it arrives.`)
        : row(buy ? "You buy" : "You sell", fm2Size(i), undefined, false),
      explained("Price limit", limit, buy ? "It never pays a higher price than this." : "It never sells at a lower price than this."),
      buy
        ? row("You pay at most", amountText(quote, i.quote.asset), undefined, false)
        : explained("You receive at least", amountText(quote - fee, i.quote.asset), "For all of it, after the seller's fee."),
      ...(buy ? [] : [explained("Seller's fee", `${amountText(fee, i.quote.asset)} at this price`, "More if it sells at a better price, in the same proportion.")]),
      row("What does not trade at once", atOnce ? "Is dropped" : i.tif === "gtc" ? "Waits in the order book" : "All of it must wait", undefined, false),
    ];
    more.unshift(
      row("Market", m.name, m.id, false),
      explained("Exactly", `${whole(BigInt(i.lots))} lots at ${whole(ticks)} ticks`, `As it is signed. One lot is ${lotText(m)}. One tick moves the price by ${tickText(m)}.`),
      row("Kind of order", tifText(i.tif), undefined, false),
    );
    el("approve").textContent = `${buy ? "Buy" : "Sell"} ${fm2Size(i)}`;
  } else if (i.action === "cancel") {
    el("title").textContent = "Cancel this order?";
    el("action-text").textContent = `The order stops waiting in the order book, and what it set aside becomes available again. Until the network confirms the cancellation, the order can still trade. ${signed}`;
    rows = [i.placed && i.placed.action === "place" ? row("Order", placeLine(i.placed), undefined, false) : row("Order", shortId(i.order, 10, 6), undefined, false)];
    more.unshift(row("Order number", shortId(i.order, 10, 6), i.order, false));
    el("approve").textContent = "Cancel the order";
  } else if (i.action === "cancel_all") {
    el("title").textContent = "Cancel all your orders?";
    el("action-text").textContent = `Every order this account has waiting stops, in every market of this network, and what they set aside becomes available again. Until the network confirms it, the orders can still trade. ${signed}`;
    el("approve").textContent = "Cancel all orders";
  } else if (i.action === "transfer") {
    const text = amountText(BigInt(i.amount.atoms), i.amount.asset);
    el("title").textContent = `Send ${text}?`;
    el("action-text").textContent = `It goes to another account on this test network, with no fee. It stays inside FlowMesh: nothing moves on the B3 chain. Once the network confirms it, it cannot be taken back, so check the account number. ${signed}`;
    rows = [row("You send", text, undefined, false), row("To account", shortId(i.to, 10, 6), i.to, false)];
    el("approve").textContent = `Send ${text}`;
  } else {
    // The core drafts only what this window can show; never approve blind.
    el("title").textContent = "This request cannot be shown";
    el("action-text").textContent = "This version of B3 Hive does not know this kind of action. Reject it.";
    el("approve").hidden = true;
    more.length = 0;
  }
  el("action-rows").replaceChildren(...rows);
  el("action-more-rows").replaceChildren(...more);
}

/** How an approval's button looks. Red marks a request that moves value
 *  out of this wallet or its trading balance. A FlowMesh buy is the
 *  exception: it wears the green that buying has on the Trade page, so the
 *  button pressed there and the one here are plainly the same. */
function approveClass(r: Request): string {
  if (r.kind === "fm2" && r.action.intent.action === "place" && r.action.intent.side === "buy") return "btn btn-buy";
  return `btn ${movesValue(r) ? "btn-danger" : "btn-primary"}`;
}

/** Whether approving moves value out of this wallet or its trading
 *  balance: such requests get the red button, the rest the ordinary one. */
function movesValue(r: Request): boolean {
  switch (r.kind) {
    case "pay":
      return r.summary.outputs.some((o) => !o.change && !o.mine);
    case "send_psbt":
      return true;
    case "fm2":
      return r.action.intent.action === "place" || r.action.intent.action === "transfer";
    default:
      return false;
  }
}

// As Qt's send confirmation, Approve is held back briefly when a request
// appears, so a click or Enter meant for another window cannot approve it.
const ARM_MS = 2000;
let armedAt = 0;
let armTimer: number | undefined;

function arm(): void {
  armedAt = Date.now() + ARM_MS;
  el<HTMLButtonElement>("approve").disabled = true;
  window.clearTimeout(armTimer);
  armTimer = window.setTimeout(() => {
    if (!deciding) el<HTMLButtonElement>("approve").disabled = false;
  }, ARM_MS);
}

function show(draft: DraftView | null): void {
  el("empty").hidden = draft !== null;
  el("draft").hidden = draft === null;
  el("error").hidden = true;
  if (!draft) return;
  const r = draft.request;
  const pay = r.kind === "pay" || r.kind === "release" || r.kind === "sign_psbt" || r.kind === "send_psbt";
  el("pay-body").hidden = !pay;
  el("action-body").hidden = pay;
  el("approve").hidden = false;
  if (r.kind === "pay") showPay(r.summary);
  else if (r.kind === "release") showRelease(r.summary, r.producers);
  else if (r.kind === "sign_psbt" || r.kind === "send_psbt") showPsbt(r.kind, r.summary);
  else if (r.kind === "fm2") showFm2(r.action);
  else showAction(r);
  el("approve").className = approveClass(r);
  // An action's rows and its details show only when the request filled
  // them in.
  el("action-rows").hidden = pay || el("action-rows").childElementCount === 0;
  el("action-more").hidden = r.kind !== "fm2" || el("action-more-rows").childElementCount === 0;
  el<HTMLDetailsElement>("action-more").open = false;
  arm();
  el("pass-field").hidden = !draft.locked;
  el<HTMLInputElement>("pass").value = "";
  // Start at the top, so the explanation is read before the passphrase
  // field; focusing it must not scroll the text away.
  document.querySelector("#draft .modal-body")?.scrollTo({ top: 0 });
  if (draft.locked) el("pass").focus({ preventScroll: true });
}

let current: DraftView | null = null;
let deciding = false;

function setBusy(busy: boolean): void {
  deciding = busy;
  el<HTMLButtonElement>("approve").disabled = busy || Date.now() < armedAt;
  el<HTMLButtonElement>("reject").disabled = busy;
}

// After a decision the core returns the next waiting draft, if any, and
// closes this window itself when nothing is left.
async function decide(approve: boolean): Promise<void> {
  if (!current || deciding) return;
  if (approve && (Date.now() < armedAt || el("approve").hidden)) return;
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
    if (current?.locked) el("pass").focus({ preventScroll: true });
  } finally {
    setBusy(false);
  }
}

el("approve").addEventListener("click", () => decide(true));
el("reject").addEventListener("click", () => decide(false));
// Enter in the passphrase field approves, as it would in a form.
el("pass").addEventListener("keydown", (e) => {
  if (e.key === "Enter" && el<HTMLInputElement>("pass").value) {
    e.preventDefault();
    void decide(true);
  }
});
current = await invoke<DraftView | null>("approval_pending");
show(current);
