// The Trade page's FlowMesh V2 regtest API card: read-only status, markets,
// totals and account balances from an operator's HTTPS API on this machine
// (src-tauri/src/v2api.rs), kept live from its certified-state stream. The
// V2 API has no book, clearing or trade route, so none are shown. Nothing
// here signs or submits.
import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";

import { bannerTone, el } from "../shared/ui";
import {
  accountText,
  assetName,
  atoms,
  isHash,
  liveText,
  statusText,
  totalsNote,
  type V2Account,
  type V2Event,
  type V2Info,
  type V2Totals,
} from "./v2fmt";

let info: V2Info | null = null;
let liveHeight: number | null = null;
let receivedAt: number | null = null;
let liveError: string | null = null;
let active = false;
let busy = false;
let ticker: number | undefined;

function showError(text: string): void {
  const box = el("v2-error");
  box.textContent = text;
  box.hidden = !text;
  if (text) bannerTone(box, "danger");
}

function cell(text: string, numeric = false, mono = false): HTMLTableCellElement {
  const td = document.createElement("td");
  td.textContent = text;
  if (numeric) td.className = "num tnum";
  else if (mono) td.className = "mono";
  return td;
}

function row(cells: HTMLTableCellElement[]): HTMLTableRowElement {
  const tr = document.createElement("tr");
  tr.append(...cells);
  return tr;
}

function renderConnected(): void {
  const connected = info !== null;
  el("v2-view").hidden = !connected;
  el("v2-disconnect").hidden = !connected;
  const badge = el("v2-badge");
  badge.textContent = connected ? "Connected, read-only" : "Not connected";
  badge.className = connected ? "badge badge-info" : "badge";
  for (const id of ["v2-url", "v2-cert", "v2-pin"]) el<HTMLInputElement>(id).disabled = connected;
  el<HTMLButtonElement>("v2-connect").hidden = connected;
  if (!info) return;
  el("v2-status").textContent = statusText(info);
  const current = info;
  el("v2-markets").replaceChildren(
    ...info.markets.map((m) =>
      row([cell(m.name), cell(assetName(current, m.base)), cell(assetName(current, m.quote)), cell(atoms(m.base_atoms_per_lot), true), cell(atoms(m.quote_atoms_per_tick), true)]),
    ),
  );
  renderLive();
}

function renderTotals(totals: V2Totals): void {
  el("v2-totals").replaceChildren(
    ...totals.assets.map((a) =>
      row([
        cell(assetName(info, a.asset)),
        cell(atoms(a.deposits), true),
        cell(atoms(a.available), true),
        cell(atoms(a.reserved), true),
        cell(atoms(a.pending), true),
        cell(atoms(a.fees_held), true),
        cell(atoms(a.total_liabilities), true),
      ]),
    ),
  );
  el("v2-totals-note").textContent = totalsNote(totals);
}

function renderLive(): void {
  el("v2-live").textContent = liveText(liveHeight, receivedAt, Date.now(), liveError);
}

async function connect(): Promise<void> {
  if (busy) return;
  busy = true;
  showError("");
  try {
    info = await invoke<V2Info>("v2_connect", {
      url: el<HTMLInputElement>("v2-url").value.trim(),
      certificate: el<HTMLInputElement>("v2-cert").value.trim(),
      certificateSha256: el<HTMLInputElement>("v2-pin").value.trim().toLowerCase(),
    });
    liveHeight = receivedAt = null;
    liveError = null;
    renderConnected();
    renderTotals(await invoke<V2Totals>("v2_totals"));
    if (active) await invoke("v2_watch", { after: info.height });
  } catch (e) {
    showError(String(e));
  } finally {
    busy = false;
  }
}

async function disconnect(): Promise<void> {
  info = null;
  liveHeight = receivedAt = null;
  el("v2-totals").replaceChildren();
  el("v2-balances").replaceChildren();
  el("v2-balances-wrap").hidden = true;
  el("v2-account-status").textContent = "";
  renderConnected();
  try {
    await invoke("v2_disconnect");
  } catch (e) {
    showError(String(e));
  }
}

async function lookUp(): Promise<void> {
  const account = el<HTMLInputElement>("v2-account").value.trim().toLowerCase();
  const status = el("v2-account-status");
  const wrap = el("v2-balances-wrap");
  if (!isHash(account)) {
    status.textContent = "An account is 64 hexadecimal characters.";
    wrap.hidden = true;
    return;
  }
  status.textContent = "Reading…";
  try {
    const a = await invoke<V2Account>("v2_account", { account });
    status.textContent = accountText(a);
    el("v2-balances").replaceChildren(...a.balances.map((b) => row([cell(assetName(info, b.asset)), cell(atoms(b.available), true), cell(atoms(b.reserved), true)])));
    wrap.hidden = !a.found;
  } catch (e) {
    status.textContent = String(e);
    wrap.hidden = true;
  }
}

/** The Trade page opened: follow the stream again if connected. */
export function startV2(): void {
  active = true;
  window.clearInterval(ticker);
  ticker = window.setInterval(() => {
    if (info) renderLive();
  }, 1000);
  if (info) void invoke("v2_watch", { after: liveHeight ?? info.height }).catch((e) => showError(String(e)));
}

/** The Trade page closed: stop following the stream; the connection stays. */
export function stopV2(): void {
  if (!active) return;
  active = false;
  window.clearInterval(ticker);
  ticker = undefined;
  if (info) void invoke("v2_unwatch").catch(() => undefined);
}

el("v2-form").addEventListener("submit", (event) => {
  event.preventDefault();
  void connect();
});
el("v2-disconnect").addEventListener("click", () => void disconnect());
el("v2-account-form").addEventListener("submit", (event) => {
  event.preventDefault();
  void lookUp();
});

void listen<V2Event>("v2-certified-state", (event) => {
  if (!info) return;
  const e = event.payload;
  if (e.error) {
    liveError = e.error;
  } else if (e.totals) {
    liveError = null;
    liveHeight = e.height;
    receivedAt = Date.now();
    renderTotals(e.totals);
  }
  renderLive();
});
