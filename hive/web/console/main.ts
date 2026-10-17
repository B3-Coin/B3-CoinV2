// The console window. Lines go to the core, which parses and runs them like
// the Qt console. Only the filtered form of each line (secrets replaced by
// "(…)") is shown back or kept in the history.
import { invoke } from "@tauri-apps/api/core";

type Reply = { command: string; ok: boolean; output: string };

function el<T extends HTMLElement>(id: string): T {
  const node = document.getElementById(id);
  if (!node) throw new Error(`missing #${id}`);
  return node as T;
}

const log = el("log");
const input = el<HTMLInputElement>("line");
const walletSelect = el<HTMLSelectElement>("wallet");
const history: string[] = [];
let historyPos = 0;
let busy = false;
let lastWallet: string | null | undefined;

// Text only: replies are node output and may contain attacker-chosen strings.
function write(text: string, kind: "request" | "reply" | "error" | "warning" | "note"): HTMLElement {
  const entry = document.createElement("div");
  entry.className = `entry ${kind}`;
  entry.textContent = text;
  log.append(entry);
  log.scrollTop = log.scrollHeight;
  return entry;
}

function welcome(): void {
  write(
    "Welcome to the B3 Hive RPC console.\n" +
      "Use up and down arrows to navigate history, and Ctrl+L to clear screen.\n" +
      "Type help for an overview of available commands.\n" +
      "For more information on using this console, type help-console.",
    "note",
  );
  write(
    "WARNING: Scammers have been active, telling users to type commands here, stealing their wallet " +
      "contents. Do not use this console without fully understanding the ramifications of a command.",
    "warning",
  );
}

function selectedWallet(): string | null {
  return walletSelect.value === "" ? null : walletSelect.value;
}

async function loadWallets(): Promise<void> {
  try {
    const wallets = await invoke<string[]>("console_wallets");
    const keep = walletSelect.value;
    const none = new Option("(no wallet)", "");
    walletSelect.replaceChildren(none, ...wallets.map((w) => new Option(w, w)));
    // As in Qt: with exactly one wallet loaded, commands use it.
    walletSelect.value = wallets.includes(keep) ? keep : wallets.length === 1 ? wallets[0] : "";
  } catch {
    walletSelect.replaceChildren(new Option("(no wallet)", ""));
  }
}

async function submit(line: string): Promise<void> {
  const wallet = selectedWallet();
  let reply: Reply;
  try {
    reply = await invoke<Reply>("console_exec", { line, wallet });
  } catch (e) {
    // Refused before running: keep the line so it can be fixed.
    write(String(e), "error");
    return;
  }
  input.value = "";
  if (wallet !== lastWallet) {
    write(wallet ? `Executing command using "${wallet}" wallet` : "Executing command without any wallet", "note");
    lastWallet = wallet;
  }
  write(reply.command, "request");
  if (history[history.length - 1] !== reply.command) history.push(reply.command);
  historyPos = history.length;
  write(reply.output, reply.ok ? "reply" : "error");
  // Commands such as loadwallet change the wallet list.
  void loadWallets();
}

el("prompt").addEventListener("submit", async (event) => {
  event.preventDefault();
  const line = input.value.trim();
  if (!line) return;
  // Like Qt, one command at a time, except stop.
  if (busy && line !== "stop") return;
  busy = true;
  input.setAttribute("aria-busy", "true");
  try {
    await submit(line);
  } finally {
    busy = false;
    input.removeAttribute("aria-busy");
  }
});

input.addEventListener("keydown", (event) => {
  if (event.key === "ArrowUp" && historyPos > 0) {
    historyPos -= 1;
    input.value = history[historyPos];
    event.preventDefault();
  } else if (event.key === "ArrowDown" && historyPos < history.length) {
    historyPos += 1;
    input.value = history[historyPos] ?? "";
    event.preventDefault();
  }
});

function clear(): void {
  log.replaceChildren();
  welcome();
}

document.addEventListener("keydown", (event) => {
  if (event.ctrlKey && event.key.toLowerCase() === "l") {
    event.preventDefault();
    clear();
  }
});
el("clear").addEventListener("click", clear);

welcome();
await loadWallets();
