// The address picker: one dialog that lists the address book and the
// wallet's own addresses, so an address is chosen instead of typed. It only
// fills a field; whatever is paid is still checked and approved as usual.

import { invoke } from "@tauri-apps/api/core";
import { amountNode, el, icon } from "../shared/ui";
import { trapDialog } from "./a11y";

export type Picked = { address: string; label: string; mine: boolean };
type Tab = "book" | "mine";

// Mirror book::Contact and receive::Receiving.
type Contact = { address: string; label: string };
type Own = { address: string; label: string; balance: string; coins: number; change: boolean; transactions: number };

/** The order Receive and the picker list own addresses in. The node keeps
 *  no creation order, so: labelled addresses A to Z, then unlabelled ones,
 *  then change. */
export function sortOwn<T extends { address: string; label: string; change: boolean }>(list: T[]): T[] {
  const rank = (a: T) => (a.change ? 2 : a.label ? 0 : 1);
  return [...list].sort(
    (x, y) =>
      rank(x) - rank(y) ||
      x.label.localeCompare(y.label, undefined, { sensitivity: "base" }) ||
      x.address.localeCompare(y.address),
  );
}

let resolvePick: ((p: Picked | null) => void) | null = null;
let tab: Tab = "book";
let contacts: Contact[] | string = [];
let own: Own[] | string = [];
let returnFocus: HTMLElement | null = null;
let release: (() => void) | null = null;
// Each opening loads the lists anew; a slower earlier load must not win.
let opening = 0;

function finish(p: Picked | null): void {
  el("pick-backdrop").hidden = true;
  release?.();
  release = null;
  const done = resolvePick;
  resolvePick = null;
  returnFocus?.focus();
  done?.(p);
}

function row(label: string, address: string, badge: string | null, trail: Node | null, pick: Picked): HTMLElement {
  const b = document.createElement("button");
  b.type = "button";
  b.className = "list-row clickable pick-row";
  const lead = document.createElement("div");
  lead.className = pick.mine ? "lead in" : "lead out";
  lead.append(icon(pick.mine ? "arrow-down" : "users"));
  const body = document.createElement("div");
  body.className = "body";
  const title = document.createElement("span");
  title.className = "row gap-2";
  const name = document.createElement("span");
  name.textContent = label || "No label";
  if (!label) name.className = "muted";
  title.append(name);
  if (badge) {
    const tag = document.createElement("span");
    tag.className = "badge badge-neutral";
    tag.textContent = badge;
    title.append(tag);
  }
  const meta = document.createElement("span");
  meta.className = "meta is-mono pick-addr";
  meta.textContent = address;
  body.append(title, meta);
  b.append(lead, body);
  if (trail) {
    const t = document.createElement("div");
    t.className = "trail";
    t.append(trail);
    b.append(t);
  }
  b.addEventListener("click", () => finish(pick));
  return b;
}

function render(): void {
  for (const t of ["book", "mine"] as const) el(`pick-tab-${t}`).setAttribute("aria-selected", String(t === tab));
  const q = el<HTMLInputElement>("pick-search").value.trim().toLowerCase();
  const match = (a: { label: string; address: string }) =>
    !q || a.label.toLowerCase().includes(q) || a.address.toLowerCase().includes(q);
  const source = tab === "book" ? contacts : own;
  let rows: HTMLElement[] = [];
  let empty = "";
  if (typeof source === "string") {
    empty = source;
  } else if (tab === "book") {
    rows = (source as Contact[])
      .filter(match)
      .map((c) => row(c.label, c.address, null, null, { address: c.address, label: c.label, mine: false }));
    empty = source.length
      ? "No contact matches."
      : "No contacts yet. Addresses you pay with a label, or save on Address book, appear here.";
  } else {
    rows = (source as Own[])
      .filter(match)
      .map((a) =>
        row(
          a.label,
          a.address,
          a.change ? (a.label ? "Wallet's own" : "Change") : a.transactions === 0 && a.coins === 0 ? "Unused" : null,
          a.coins ? amountNode(a.balance) : null,
          { address: a.address, label: a.label, mine: true },
        ),
      );
    empty = source.length ? "No address matches." : "This wallet has no addresses yet.";
  }
  el("pick-list").replaceChildren(...rows);
  el("pick-empty").hidden = rows.length > 0;
  el("pick-empty").textContent = empty;
}

function show(t: Tab): void {
  tab = t;
  render();
  el<HTMLInputElement>("pick-search").focus();
}

/** Opens the picker; resolves with the chosen address, or null if closed. */
export async function pickAddress(title: string, sub: string, start: Tab): Promise<Picked | null> {
  if (resolvePick) finish(null);
  returnFocus = document.activeElement as HTMLElement | null;
  el("pick-title").textContent = title;
  el("pick-sub").textContent = sub;
  el<HTMLInputElement>("pick-search").value = "";
  contacts = "Loading…";
  own = "Loading…";
  tab = start;
  el("pick-backdrop").hidden = false;
  release ??= trapDialog(el("pick-backdrop"));
  render();
  el<HTMLInputElement>("pick-search").focus();
  const result = new Promise<Picked | null>((resolve) => (resolvePick = resolve));
  const mineOpening = ++opening;
  const [book, mine] = await Promise.allSettled([invoke<Contact[]>("book_list"), invoke<Own[]>("receive_addresses")]);
  if (mineOpening !== opening) return result;
  contacts = book.status === "fulfilled" ? book.value : String(book.reason);
  own = mine.status === "fulfilled" ? sortOwn(mine.value) : String(mine.reason);
  if (resolvePick) render();
  return result;
}

el("pick-close").addEventListener("click", () => finish(null));
el("pick-backdrop").addEventListener("click", (e) => {
  if (e.target === el("pick-backdrop")) finish(null);
});
el("pick-backdrop").addEventListener("keydown", (e) => {
  if (e.key === "Escape") finish(null);
});
el("pick-tab-book").addEventListener("click", () => show("book"));
el("pick-tab-mine").addEventListener("click", () => show("mine"));
el("pick-search").addEventListener("input", render);
