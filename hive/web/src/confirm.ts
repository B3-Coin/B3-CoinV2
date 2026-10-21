// A question the main window asks before something that interrupts the
// node or cannot be undone. Cancel has the focus, so Enter does not agree.
import { el } from "../shared/ui";
import { trapDialog } from "./a11y";

let answer: ((yes: boolean) => void) | null = null;
let release: (() => void) | null = null;
let returnFocus: HTMLElement | null = null;

function finish(yes: boolean): void {
  el("confirm-backdrop").hidden = true;
  release?.();
  release = null;
  const done = answer;
  answer = null;
  returnFocus?.focus();
  done?.(yes);
}

/** Resolves true only when the user picks `yes`. */
export function confirmAction(title: string, lines: string[], yes: string, danger = false): Promise<boolean> {
  if (answer) finish(false);
  returnFocus = document.activeElement as HTMLElement | null;
  el("confirm-title").textContent = title;
  el("confirm-text").replaceChildren(
    ...lines.map((line) => {
      const p = document.createElement("p");
      p.textContent = line;
      return p;
    }),
  );
  const button = el<HTMLButtonElement>("confirm-yes");
  button.textContent = yes;
  button.className = `btn ${danger ? "btn-danger" : "btn-primary"}`;
  el("confirm-backdrop").hidden = false;
  release = trapDialog(el("confirm-backdrop"));
  el("confirm-no").focus();
  return new Promise((resolve) => (answer = resolve));
}

el("confirm-yes").addEventListener("click", () => finish(true));
el("confirm-no").addEventListener("click", () => finish(false));
el("confirm-backdrop").addEventListener("click", (e) => {
  if (e.target === el("confirm-backdrop")) finish(false);
});
el("confirm-backdrop").addEventListener("keydown", (e) => {
  if (e.key === "Escape") finish(false);
});
