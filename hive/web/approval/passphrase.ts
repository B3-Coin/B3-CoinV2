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

  el("form").addEventListener("submit", async (e) => {
    e.preventDefault();
    fail("");
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
