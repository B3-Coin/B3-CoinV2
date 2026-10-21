// The passphrase window. It shows its one request (encrypt, change the
// passphrase, or unlock just to start staking), hands the typed
// passphrases to the core and closes; nothing typed here reaches another
// window. Its only powers are reading the request and submitting to it.
import { invoke } from "@tauri-apps/api/core";
import { el, initTheme } from "../shared/ui";

type Request = { id: string; kind: "encrypt" | "change" | "stake"; wallet: string };

await initTheme();

const request = await invoke<Request | null>("passphrase_pending");
el("empty").hidden = request !== null;
el("form").hidden = request === null;

function fail(text: string): void {
  el("error").hidden = !text;
  el("error").textContent = text;
  if (text) el("error").scrollIntoView({ block: "nearest" });
}

function clear(): void {
  for (const id of ["old", "new", "repeat"]) el<HTMLInputElement>(id).value = "";
  strength();
}

// The core refuses fewer characters (passphrase::MIN_CHARS).
const MIN_CHARS = 10;

/** A rough guess at how hard the passphrase is to guess: words count as
 *  common words, characters by the kinds used. It only guides; the core
 *  enforces the minimum length alone. */
function strength(): void {
  const text = el<HTMLInputElement>("new").value;
  const length = [...text].length;
  const words = text.trim().split(/\s+/).filter(Boolean).length;
  let pool = 0;
  if (/[a-z]/.test(text)) pool += 26;
  if (/[A-Z]/.test(text)) pool += 26;
  if (/\d/.test(text)) pool += 10;
  if (/[^a-zA-Z\d]/.test(text)) pool += 33;
  const bits = Math.max(words >= 4 ? words * 11 : 0, pool ? length * Math.log2(pool) * 0.6 : 0);
  const score = length === 0 ? 0 : length < MIN_CHARS ? 1 : bits < 50 ? 2 : bits < 75 ? 3 : 4;
  const meter = el("strength");
  meter.className = `strength s${score}`;
  [...meter.children].forEach((bar, i) => bar.classList.toggle("on", i < score));
  el("strength-text").textContent = [
    "",
    `Too short: ${MIN_CHARS - length} more character${MIN_CHARS - length === 1 ? "" : "s"} at least.`,
    "Weak: add words or random characters.",
    "Fair.",
    "Strong.",
  ][score];
}

if (request) {
  const encrypt = request.kind === "encrypt";
  const stake = request.kind === "stake";
  const wallet = request.wallet === "" ? "the default wallet" : `wallet ${request.wallet}`;
  el("title").textContent = encrypt ? "Encrypt wallet" : stake ? "Start staking" : "Change passphrase";
  el("wallet").textContent = `For ${wallet}`;
  el("intro").textContent = encrypt
    ? "Sending will need this passphrase from now on. Encrypting starts a new key chain, so make a new backup afterwards: older backups do not hold the keys for new addresses."
    : stake
      ? "The wallet unlocks only to start staking and locks again straight after. Staking goes on while it is locked."
      : "Enter the current passphrase, then the new one twice.";
  el("encrypt-warning").hidden = !encrypt;
  el("ack-row").hidden = !encrypt;
  el("old-field").hidden = encrypt;
  el("new-field").hidden = stake;
  el("repeat-field").hidden = stake;
  if (stake) el("old-label").textContent = "Wallet passphrase";
  el("submit").textContent = encrypt ? "Encrypt wallet" : stake ? "Start staking" : "Change passphrase";
  el<HTMLInputElement>(encrypt ? "new" : "old").focus();
  el("new").addEventListener("input", strength);

  el("form").addEventListener("submit", async (e) => {
    e.preventDefault();
    fail("");
    if (!stake && [...el<HTMLInputElement>("new").value].length < MIN_CHARS) {
      fail(`Use at least ${MIN_CHARS} characters: ten or more random characters, or several words.`);
      return;
    }
    if (encrypt && !el<HTMLInputElement>("ack").checked) {
      fail("Tick the box to confirm you understand the risk.");
      return;
    }
    const submit = el<HTMLButtonElement>("submit");
    submit.disabled = true;
    try {
      // The core closes this window on success.
      await invoke("passphrase_submit", {
        id: request.id,
        entry: {
          old: el<HTMLInputElement>("old").value,
          new: el<HTMLInputElement>("new").value,
          repeat: el<HTMLInputElement>("repeat").value,
        },
      });
      clear();
    } catch (err) {
      fail(String(err));
    } finally {
      submit.disabled = false;
    }
  });
}

el("cancel").addEventListener("click", () => {
  clear();
  void invoke("passphrase_cancel");
});
