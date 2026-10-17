// Helpers shared by every window. Everything here builds DOM nodes and sets
// text; nothing goes through innerHTML, because node replies can carry
// attacker-chosen strings.
import { invoke } from "@tauri-apps/api/core";

export function el<T extends HTMLElement>(id: string): T {
  const node = document.getElementById(id);
  if (!node) throw new Error(`missing #${id}`);
  return node as T;
}

const SVG = "http://www.w3.org/2000/svg";

/** An icon from public/icons.svg, e.g. icon("home", "i-sm"). */
export function icon(name: string, size = "i"): SVGSVGElement {
  const svg = document.createElementNS(SVG, "svg");
  svg.setAttribute("class", size);
  svg.setAttribute("aria-hidden", "true");
  const use = document.createElementNS(SVG, "use");
  use.setAttribute("href", `/icons.svg#i-${name}`);
  svg.append(use);
  return svg;
}

// ---- amounts ---------------------------------------------------------------
// Ported from b3_docker frontend/js/core/format.js. B3 shows 9 decimals and
// the core sends amounts as exact decimal strings; these functions only
// rearrange those characters and never turn them into floating point.

const GROUP = " "; // narrow no-break space between digit groups

type Parts = { neg: boolean; int: string; frac: string };

function parts(value: string): Parts | null {
  let s = value.trim();
  if (!/^[+-]?\d+(\.\d*)?$/.test(s)) return null;
  const neg = s.startsWith("-");
  if (neg || s.startsWith("+")) s = s.slice(1);
  const [int = "0", frac = ""] = s.split(".");
  return {
    neg,
    int: int.replace(/^0+(?=\d)/, ""),
    frac: (frac + "000000000").slice(0, 9),
  };
}

function groupInt(digits: string): string {
  return digits.replace(/\B(?=(\d{3})+(?!\d))/g, GROUP);
}

function groupFrac(digits: string): string {
  return digits.replace(/(\d{3})(?=\d)/g, "$1" + GROUP);
}

/** Drops trailing zeros but keeps at least `min` decimals. */
function trimFrac(frac: string, min: number): string {
  let f = frac.replace(/0+$/, "");
  while (f.length < min) f += "0";
  return f;
}

/** Plain text, e.g. "1 234.5 B3". */
export function formatAmount(value: string, minDecimals = 2): string {
  const p = parts(value);
  if (!p) return "—";
  const frac = trimFrac(p.frac, minDecimals);
  return `${p.neg ? "−" : ""}${groupInt(p.int)}${frac ? "." + groupFrac(frac) : ""} B3`;
}

/** The amount treatment from b3_docker: whole part bold, fraction smaller
 *  and muted, then the unit. `extra` adds a size class such as amt-hero. */
export function amountNode(value: string, extra = "", minDecimals = 2): HTMLSpanElement {
  const span = document.createElement("span");
  span.className = ["amt", extra].filter(Boolean).join(" ");
  const p = parts(value);
  if (!p) {
    span.classList.add("muted");
    span.textContent = "—";
    return span;
  }
  const add = (cls: string, text: string) => {
    const part = document.createElement("span");
    part.className = cls;
    part.textContent = text;
    span.append(part);
  };
  if (p.neg) add("amt-sign", "−");
  add("amt-int", groupInt(p.int));
  const frac = trimFrac(p.frac, minDecimals);
  if (frac) add("amt-frac", "." + groupFrac(frac));
  add("amt-unit", "B3");
  return span;
}

/** Exact sum of decimal amount strings, through integer base units (1 B3 =
 *  10^9). Returns null if any value is not a plain decimal. */
export function sumAmounts(values: string[]): string | null {
  let total = 0n;
  for (const value of values) {
    const p = parts(value);
    if (!p) return null;
    const units = BigInt(p.int) * 1_000_000_000n + BigInt(p.frac);
    total += p.neg ? -units : units;
  }
  const neg = total < 0n;
  const abs = neg ? -total : total;
  const frac = (abs % 1_000_000_000n).toString().padStart(9, "0");
  return `${neg ? "-" : ""}${abs / 1_000_000_000n}.${frac}`;
}

/** First 8 … last 6 characters; the full value goes in a title. */
export function shorten(text: string, head = 8, tail = 6): string {
  return text.length <= head + tail + 1 ? text : `${text.slice(0, head)}…${text.slice(-tail)}`;
}

// ---- theme -----------------------------------------------------------------

export type Theme = "light" | "dark";

/** Follows the system light/dark setting until the user picks one in the
 *  main window. The core keeps that pick for the app's lifetime, so windows
 *  opened later start in it too (incognito windows share no storage). */
export async function initTheme(): Promise<Theme> {
  const root = document.documentElement;
  const media = window.matchMedia("(prefers-color-scheme: light)");
  const follow = () => {
    if (!root.dataset.themePicked) root.dataset.theme = media.matches ? "light" : "dark";
  };
  media.addEventListener("change", follow);
  follow();
  try {
    const picked = await invoke<Theme | null>("ui_theme");
    if (picked) {
      root.dataset.theme = picked;
      root.dataset.themePicked = "1";
    }
  } catch {
    // Keep following the system.
  }
  return root.dataset.theme as Theme;
}

/** Main window only: switches the theme and tells the core. */
export async function toggleTheme(): Promise<Theme> {
  const root = document.documentElement;
  const next: Theme = root.dataset.theme === "light" ? "dark" : "light";
  root.dataset.theme = next;
  root.dataset.themePicked = "1";
  await invoke("set_ui_theme", { theme: next });
  return next;
}
