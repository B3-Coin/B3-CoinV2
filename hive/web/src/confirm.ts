// A question the main window asks before something that interrupts the
// node or cannot be undone. Cancel has the focus, so Enter does not agree.
import { el } from "../shared/ui";
import { trapDialog } from "./a11y";

export type Choice = { label: string; danger?: boolean };

let answer: ((choice: number | null) => void) | null = null;
let release: (() => void) | null = null;
let returnFocus: HTMLElement | null = null;

function finish(choice: number | null): void {
  el("confirm-backdrop").hidden = true;
  for (const b of el("confirm-foot").querySelectorAll(".confirm-choice")) b.remove();
  release?.();
  release = null;
  const done = answer;
  answer = null;
  returnFocus?.focus();
  done?.(choice);
}

/** Puts the question with its choices after Cancel. Resolves to the index
 *  of the choice taken, or null for Cancel, Escape or a click outside. */
export function chooseAction(title: string, lines: string[], choices: Choice[]): Promise<number | null> {
  if (answer) finish(null);
  returnFocus = document.activeElement as HTMLElement | null;
  el("confirm-title").textContent = title;
  el("confirm-text").replaceChildren(
    ...lines.map((line) => {
      const p = document.createElement("p");
      p.textContent = line;
      return p;
    }),
  );
  el("confirm-foot").append(
    ...choices.map((c, i) => {
      const button = document.createElement("button");
      button.type = "button";
      button.className = `btn confirm-choice ${c.danger ? "btn-danger" : i === 0 ? "btn-primary" : "btn-secondary"}`;
      button.textContent = c.label;
      button.addEventListener("click", () => finish(i));
      return button;
    }),
  );
  el("confirm-backdrop").hidden = false;
  release = trapDialog(el("confirm-backdrop"));
  el("confirm-no").focus();
  return new Promise((resolve) => (answer = resolve));
}

/** Resolves true only when the user picks `yes`. */
export async function confirmAction(title: string, lines: string[], yes: string, danger = false): Promise<boolean> {
  return (await chooseAction(title, lines, [{ label: yes, danger }])) === 0;
}

el("confirm-no").addEventListener("click", () => finish(null));
el("confirm-backdrop").addEventListener("click", (e) => {
  if (e.target === el("confirm-backdrop")) finish(null);
});
el("confirm-backdrop").addEventListener("keydown", (e) => {
  if (e.key === "Escape") finish(null);
});
