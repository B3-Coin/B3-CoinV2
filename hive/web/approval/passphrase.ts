// The passphrase window. It shows its one request, hands the typed
// passphrases to the core and closes; nothing typed here reaches another
// window. Its only powers are reading the request and submitting to it.
import { invoke } from "@tauri-apps/api/core";
import { el, initTheme } from "../shared/ui";

type Request = { id: string; kind: "encrypt" | "change"; wallet: string };

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
  const wallet = request.wallet === "" ? "the default wallet" : `wallet ${request.wallet}`;
  el("title").textContent = encrypt ? "Encrypt wallet" : "Change passphrase";
  el("wallet").textContent = `For ${wallet}`;
  el("intro").textContent = encrypt
    ? "Sending will need this passphrase from now on. Encrypting starts a new key chain, so make a new backup afterwards: older backups do not hold the keys for new addresses."
    : "Enter the current passphrase, then the new one twice.";
  el("encrypt-warning").hidden = !encrypt;
  el("ack-row").hidden = !encrypt;
  el("old-field").hidden = encrypt;
  el("submit").textContent = encrypt ? "Encrypt wallet" : "Change passphrase";
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
