// Keyboard behaviour for the main window's dialogs and tab lists.

const FOCUSABLE = "a[href], button, input, select, textarea, [tabindex]";

function focusable(root: HTMLElement): HTMLElement[] {
  return [...root.querySelectorAll<HTMLElement>(FOCUSABLE)].filter(
    (x) => x.tabIndex >= 0 && !(x as HTMLButtonElement).disabled && x.getClientRects().length > 0,
  );
}

/** While a dialog is open, everything outside `backdrop` is inert, so
 *  neither a click, Tab nor a screen reader reaches the page behind it, and
 *  Tab wraps around inside it. Returns the function that undoes this; call
 *  it before moving focus back to the page. */
export function trapDialog(backdrop: HTMLElement): () => void {
  const made: HTMLElement[] = [];
  for (let node: HTMLElement = backdrop; node.parentElement && node !== document.body; node = node.parentElement) {
    for (const sibling of node.parentElement.children) {
      if (sibling !== node && sibling instanceof HTMLElement && !sibling.inert) {
        sibling.inert = true;
        made.push(sibling);
      }
    }
  }
  const onKey = (e: KeyboardEvent) => {
    if (e.key !== "Tab") return;
    const items = focusable(backdrop);
    if (!items.length) return;
    const first = items[0];
    const last = items[items.length - 1];
    const at = document.activeElement;
    if (e.shiftKey && (at === first || !backdrop.contains(at))) {
      e.preventDefault();
      last.focus();
    } else if (!e.shiftKey && (at === last || !backdrop.contains(at))) {
      e.preventDefault();
      first.focus();
    }
  };
  backdrop.addEventListener("keydown", onKey);
  return () => {
    backdrop.removeEventListener("keydown", onKey);
    for (const x of made) x.inert = false;
  };
}

/** Tab lists take one Tab stop, the selected tab; the arrow keys, Home and
 *  End move between their tabs and select the one they reach. */
export function tabKeys(root: ParentNode = document): void {
  for (const list of root.querySelectorAll<HTMLElement>('[role="tablist"]')) {
    const tabs = () =>
      [...list.querySelectorAll<HTMLElement>('[role="tab"]')].filter(
        (t) => !t.hidden && !(t as HTMLButtonElement).disabled,
      );
    const sync = () => {
      const all = tabs();
      const selected = all.find((t) => t.getAttribute("aria-selected") === "true") ?? all[0];
      for (const t of list.querySelectorAll<HTMLElement>('[role="tab"]')) t.tabIndex = t === selected ? 0 : -1;
    };
    list.addEventListener("keydown", (e) => {
      const all = tabs();
      const i = all.indexOf(document.activeElement as HTMLElement);
      if (i < 0) return;
      const to = { ArrowRight: i + 1, ArrowLeft: i - 1, Home: 0, End: all.length - 1 }[e.key];
      if (to === undefined) return;
      e.preventDefault();
      const tab = all[(to + all.length) % all.length];
      // Selecting may move focus (the picker focuses its search field);
      // from the keyboard it stays on the tab.
      if (tab.getAttribute("aria-selected") !== "true") tab.click();
      tab.focus();
    });
    new MutationObserver(sync).observe(list, {
      subtree: true,
      attributes: true,
      attributeFilter: ["aria-selected", "hidden", "disabled"],
    });
    sync();
  }
}
