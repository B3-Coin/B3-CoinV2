// The approval window. It shows only what the core decoded from the draft it
// holds, and its only powers are reading that draft and deciding on it.
import { invoke } from "@tauri-apps/api/core";
import { count, el, formatAmount, initTheme, shorten, sumAmounts } from "../shared/ui";
import { formatB3, formatPrice, formatQuantity, quantityPlaces, type Units } from "../src/tradefmt";

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
// Mirror flowmesh::OrderDraft and CancelDraft: amounts are exact integer
// strings, prices in B3 atoms per base unit and quantities in base units.
type Metadata = { decimals: number | null; ticker: string; name: string; test_only: boolean };
type Point = { price: string; quantity: string };
type Order = { side: "bid" | "ask"; remaining: string; reserved: string; points: Point[] };
type OrderDraft = {
  chain: string;
  wallet: string;
  market_id: string;
  base_asset_id: string;
  metadata: Metadata;
  price_tick: string;
  side: "bid" | "ask";
  steps: Point[];
  sequence: number;
  reserve: string;
  available: string;
  replaces: Order | null;
  remote: boolean;
};
type CancelDraft = {
  chain: string;
  wallet: string;
  market_id: string;
  base_asset_id: string;
  metadata: Metadata;
  price_tick: string;
  side: "bid" | "ask";
  sequence: number;
  order: Order;
  remote: boolean;
};
// Mirror flowmesh::DepositDraft and WithdrawDraft: amounts are B3 atoms or
// token base units.
type DepositDraft = {
  chain: string;
  wallet: string;
  market_id: string;
  base_asset_id: string;
  metadata: Metadata;
  asset: "b3" | "token";
  amount: string;
  has_account: boolean;
};
type WithdrawDraft = {
  chain: string;
  wallet: string;
  market_id: string;
  base_asset_id: string;
  metadata: Metadata;
  asset: "b3" | "token";
  amount: string;
  available: string;
  destination: string;
  mine: boolean;
  sequence: number;
  remote: boolean;
};
// Mirrors flowmesh::PayoutDraft.
type PayoutDraft = {
  chain: string;
  wallet: string;
  market_id: string;
  base_asset_id: string;
  metadata: Metadata;
  effect_id: string;
  asset: "b3" | "token";
  amount: string;
  sequence: number;
  destination: string;
  mine: boolean;
};
// Mirrors flowmesh::CheckpointDraft.
type CheckpointDraft = {
  chain: string;
  wallet: string;
  market_id: string;
  checkpoint_id: string;
  sequence: number;
  waiting: number;
};
// Mirrors drafts::Request in src-tauri/src/drafts.rs.
type Request =
  | { kind: "pay"; summary: Summary }
  | { kind: "release"; summary: Summary; producers: Producers }
  | { kind: "sign_psbt"; summary: PsbtSummary }
  | { kind: "send_psbt"; summary: PsbtSummary }
  | { kind: "stake"; chain: string; wallet: string; amount: string; activation_depth: number; producers: Producers }
  | { kind: "bind_finality"; chain: string; wallet: string; rotate: boolean }
  | { kind: "trade_order"; order: OrderDraft }
  | { kind: "trade_cancel"; cancel: CancelDraft }
  | { kind: "trade_deposit"; deposit: DepositDraft }
  | { kind: "trade_withdraw"; withdraw: WithdrawDraft }
  | { kind: "trade_payout"; payout: PayoutDraft }
  | { kind: "trade_checkpoint"; checkpoint: CheckpointDraft };
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

function showAction(r: Extract<Request, { kind: "stake" | "bind_finality" }>): void {
  el("from").textContent = `From ${walletName(r.wallet)} on ${r.chain}`;
  const fee = row("Network fee", FEE_BOUND, undefined, false);
  if (r.kind === "stake") {
    el("title").textContent = `Stake ${formatAmount(r.amount)}?`;
    const enters = r.producers.stake_enters_at;
    el("action-text").textContent =
      `The amount is locked as a stake for this wallet's validator. It stays in this wallet, but ordinary payments do not spend it. It becomes active ${r.activation_depth} blocks after it confirms` +
      (enters !== null
        ? `, and makes blocks from block ${count(enters)} at the earliest.`
        : r.producers.frozen
          ? `. ${FROZEN_SET}, so it makes blocks only after that.`
          : ", and makes blocks once the validator set next rotates to include it.") +
      ` ${FEE_LATER}`;
    el("action-rows").replaceChildren(row("Stake", formatAmount(r.amount, 9)), fee, row("Network", r.chain, undefined, false));
    el("approve").textContent = "Stake now";
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

// A FlowMesh order or cancellation: exactly what is signed, in B3 per token
// and in tokens, with what it reserves and replaces.
function tokenName(m: Metadata, assetId: string): string {
  return m.ticker || m.name || `Token ${shorten(assetId, 6, 4)}`;
}

function tradeUnits(m: Metadata, tick: string): Units | null {
  return m.decimals === null ? null : { decimals: m.decimals, tick: BigInt(tick), lot: 1n };
}

const B3 = (atoms: string) => `${formatB3(BigInt(atoms))} B3`;

function stepRows(side: "bid" | "ask", points: Point[], u: Units, token: string): HTMLElement[] {
  const places = quantityPlaces(points.map((p) => BigInt(p.quantity)), u.decimals);
  return points.map((p) =>
    row(
      side === "bid" ? "Buy" : "Sell",
      `${formatQuantity(BigInt(p.quantity), u.decimals, places)} ${token} · limit ${formatPrice(BigInt(p.price), u)} B3`,
    ),
  );
}

function marketRows(d: { chain: string; market_id: string; base_asset_id: string; sequence: number; remote: boolean }): HTMLElement[] {
  return [
    row("Market", shorten(d.market_id, 10, 6), d.market_id, false),
    row("Token", shorten(d.base_asset_id, 10, 6), d.base_asset_id, false),
    explained("Account sequence", String(d.sequence), "Each signed request counts up, so none can be replayed."),
    explained(
      "Verified by",
      d.remote ? "Trading endpoint" : "This node's engine",
      d.remote ? "A remote service you trust checked the market data." : "Your own node checked the market data.",
    ),
    row("Network", d.chain, undefined, false),
  ];
}

function showTrade(r: Extract<Request, { kind: "trade_order" | "trade_cancel" }>): void {
  if (r.kind === "trade_order") {
    const o = r.order;
    const token = tokenName(o.metadata, o.base_asset_id);
    const u = tradeUnits(o.metadata, o.price_tick);
    const buy = o.side === "bid";
    const verb = buy ? "Buy" : "Sell";
    el("from").textContent = `From ${walletName(o.wallet)} on ${o.chain} · ${token} / B3${o.metadata.test_only ? " · test asset" : ""}`;
    if (!u) {
      // The core refuses to draft without verified decimals; never guess them.
      el("title").textContent = "This order cannot be shown";
      el("action-text").textContent = "The token's decimals are not verified. Reject it.";
      el("action-rows").replaceChildren();
      el("approve").hidden = true;
      return;
    }
    const total = o.steps.reduce((sum, p) => sum + BigInt(p.quantity), 0n);
    const amount = `${formatQuantity(total, u.decimals)} ${token}`;
    el("title").textContent =
      o.steps.length === 1
        ? `${verb} ${amount} at ${formatPrice(BigInt(o.steps[0].price), u)} B3?`
        : `${verb} ${amount} on a ladder of ${o.steps.length} prices?`;
    const replaced = o.replaces
      ? ` It replaces your open ${buy ? "buy" : "sell"} order, whose remaining ${formatQuantity(BigInt(o.replaces.remaining), u.decimals)} ${token} is not kept: this order's quantity counts from zero.`
      : "";
    el("action-text").textContent =
      `This signs one FlowMesh order and sends it to the market.${replaced} Accepted means queued, not filled: it fills only in a certified auction, at one uniform price for everyone that can be better than your limit. The order itself has no network fee; sellers pay 0.01% of the matched B3.` +
      (o.steps.length > 1 ? " A ladder is one order: its prices cannot be cancelled one by one." : "");
    el("action-rows").replaceChildren(
      ...stepRows(o.side, o.steps, u, token),
      row(buy ? "Reserves at most" : "Reserves", buy ? B3(o.reserve) : `${formatQuantity(BigInt(o.reserve), u.decimals)} ${token}`),
      row("Available to reserve", buy ? B3(o.available) : `${formatQuantity(BigInt(o.available), u.decimals)} ${token}`),
      ...marketRows(o),
    );
    el("approve").textContent = buy ? "Place buy order" : "Place sell order";
  } else {
    const c = r.cancel;
    const token = tokenName(c.metadata, c.base_asset_id);
    const u = tradeUnits(c.metadata, c.price_tick);
    const buy = c.side === "bid";
    el("from").textContent = `From ${walletName(c.wallet)} on ${c.chain} · ${token} / B3`;
    el("title").textContent = `Cancel your ${buy ? "buy" : "sell"} order?`;
    el("action-text").textContent =
      `This signs a cancellation of your open ${buy ? "buy" : "sell"} order and sends it to the market. It takes effect only once certified; until then the order can still fill. What it holds returns to your trading balance then.`;
    el("action-rows").replaceChildren(
      ...(u
        ? [
            row("Remaining", `${formatQuantity(BigInt(c.order.remaining), u.decimals)} ${token}`),
            row("Held for it", buy ? B3(c.order.reserved) : `${formatQuantity(BigInt(c.order.reserved), u.decimals)} ${token}`),
          ]
        : [row("Remaining", `${c.order.remaining} base units`)]),
      ...marketRows(c),
    );
    el("approve").textContent = "Send cancellation";
  }
  el("pass-help").textContent =
    "The wallet is locked. It unlocks only to sign this request and locks again straight after.";
}

// A deposit into a market's vault, or a request to pay trading balance out.
function showFunding(r: Extract<Request, { kind: "trade_deposit" | "trade_withdraw" | "trade_payout" }>): void {
  const d = r.kind === "trade_deposit" ? r.deposit : r.kind === "trade_withdraw" ? r.withdraw : r.payout;
  const token = tokenName(d.metadata, d.base_asset_id);
  const value = (atoms: string): string | null =>
    d.asset === "b3" ? B3(atoms) : d.metadata.decimals === null ? null : `${formatQuantity(BigInt(atoms), d.metadata.decimals)} ${token}`;
  el("from").textContent = `From ${walletName(d.wallet)} on ${d.chain} · ${token} / B3${d.metadata.test_only ? " · test asset" : ""}`;
  const amount = value(d.amount);
  if (amount === null) {
    el("title").textContent = "This request cannot be shown";
    el("action-text").textContent = "The token's decimals are not verified. Reject it.";
    el("action-rows").replaceChildren();
    el("approve").hidden = true;
    return;
  }
  const asset = d.asset === "b3" ? row("Asset", "B3 (native)", undefined, false) : row("Asset", d.base_asset_id, undefined, false);
  const market = [
    row("Market", shorten(d.market_id, 10, 6), d.market_id, false),
    row("Network", d.chain, undefined, false),
  ];
  if (r.kind === "trade_deposit") {
    const dep = r.deposit;
    el("title").textContent = `Deposit ${amount} for trading?`;
    el("action-text").textContent =
      "This signs and broadcasts a B3 transaction that moves the amount into the market's keyless vault, for your trading account. " +
      "It counts only after 31 confirmations and once the market admits it; the Trade page asks for that. " +
      "If it is slow, do not deposit again: the same deposit can be submitted for admission again. " +
      "The node picks the network fee when it builds the transaction, at most 0.1 B3 and usually far less; it is shown afterwards." +
      (dep.has_account ? "" : " This also creates the wallet's trading account key. Back up the wallet afterwards.");
    el("action-rows").replaceChildren(row("Deposit", amount), row("Network fee", FEE_BOUND, undefined, false), asset, ...market);
    el("approve").textContent = "Deposit";
  } else if (r.kind === "trade_withdraw") {
    const w = r.withdraw;
    el("title").textContent = `Request a withdrawal of ${amount}?`;
    el("action-text").textContent =
      "This signs a withdrawal request and sends it to the market. It is a request, not a payout: once certified, the amount leaves your trading balance. " +
      "After a checkpoint confirms it on the B3 chain, you publish the payout from the Trade page's Funds tab, which costs a small network fee. That can take a while. The request itself has no network fee." +
      (w.mine ? "" : " The address is not one of this wallet's; check it carefully, a payout cannot be taken back.");
    el("action-rows").replaceChildren(
      row("Withdraw", amount),
      row("Available", value(w.available) ?? w.available),
      row("Pay to", w.mine ? "This wallet" : "Another wallet", w.destination, false),
      asset,
      ...market,
      explained("Account sequence", String(w.sequence), "Each signed request counts up, so none can be replayed."),
      explained(
        "Verified by",
        w.remote ? "Trading endpoint" : "This node's engine",
        w.remote ? "A remote service you trust checked the market data." : "Your own node checked the market data.",
      ),
    );
    el("approve").textContent = "Request withdrawal";
  } else {
    const p = r.payout;
    el("title").textContent = `Publish the payout of ${amount}?`;
    el("action-text").textContent =
      "This signs and broadcasts the B3 transaction that pays your certified withdrawal out of the market's vault to the address below. " +
      "The amount and the address were fixed when the withdrawal was certified; this transaction cannot change them. " +
      "This wallet pays its network fee, which the node picks when it builds the transaction (at most 0.1 B3); it is shown afterwards.";
    el("action-rows").replaceChildren(
      row("Payout", amount),
      row("Network fee", FEE_BOUND, undefined, false),
      row("Pay to", p.mine ? "This wallet" : "Another wallet", p.destination, false),
      asset,
      ...market,
      explained("Certified in entry", String(p.sequence), "The market log entry whose certificate fixed this payout."),
      row("Payout id", shorten(p.effect_id, 10, 6), p.effect_id, false),
    );
    el("approve").textContent = "Publish payout";
  }
  el("pass-help").textContent =
    "The wallet is locked. It unlocks only to sign this request and locks again straight after.";
}

function showCheckpoint(c: CheckpointDraft): void {
  el("from").textContent = `From ${walletName(c.wallet)} on ${c.chain}`;
  el("title").textContent = "Publish the market's next checkpoint?";
  el("action-text").textContent =
    "This signs and broadcasts a B3 transaction that records the market's next certified entry on the B3 chain. " +
    `${c.waiting === 1 ? "A withdrawal of yours waits" : `${c.waiting} withdrawals of yours wait`} for checkpoints; ` +
    "once the one for its entry confirms, its payout can be published. Checkpoints land one entry at a time, so a later entry may need more than one. " +
    "Anyone may publish a checkpoint, and it moves no funds of the market. This wallet pays its network fee, which the node picks when it builds the transaction (at most 0.1 B3); it is shown afterwards. " +
    "If someone else publishes this checkpoint first, nothing is sent.";
  el("action-rows").replaceChildren(
    row("Entry", String(c.sequence), undefined, false),
    row("Network fee", FEE_BOUND, undefined, false),
    row("Market", shorten(c.market_id, 10, 6), c.market_id, false),
    row("Network", c.chain, undefined, false),
    row("Checkpoint id", shorten(c.checkpoint_id, 10, 6), c.checkpoint_id, false),
  );
  el("approve").textContent = "Publish checkpoint";
  el("pass-help").textContent =
    "The wallet is locked. It unlocks only to sign this request and locks again straight after.";
}

/** Whether approving moves value out of this wallet or its trading
 *  balance: such requests get the red button, the rest the ordinary one. */
function movesValue(r: Request): boolean {
  switch (r.kind) {
    case "pay":
      return r.summary.outputs.some((o) => !o.change && !o.mine);
    case "send_psbt":
    case "trade_order":
    case "trade_deposit":
    case "trade_withdraw":
    case "trade_payout":
      return true;
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
  else if (r.kind === "trade_order" || r.kind === "trade_cancel") showTrade(r);
  else if (r.kind === "trade_deposit" || r.kind === "trade_withdraw" || r.kind === "trade_payout") showFunding(r);
  else if (r.kind === "trade_checkpoint") showCheckpoint(r.checkpoint);
  else showAction(r);
  el("approve").className = `btn ${movesValue(r) ? "btn-danger" : "btn-primary"}`;
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
