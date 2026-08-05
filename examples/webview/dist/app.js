async function load() {
  const r = await fetch("/api/hello");
  const d = await r.json();
  document.getElementById("msg").textContent = d.message;
}
async function send() {
  const text = document.getElementById("text").value;
  const r = await fetch("/api/echo", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ text: text })
  });
  const d = await r.json();
  document.getElementById("reply").textContent = d.reply;
}
document.getElementById("send").addEventListener("click", send);

// A centered, in-page confirmation — not window.confirm() — so it matches
// the app's own look. Resolves true/false; never rejects.
function confirmDialog(message) {
  return new Promise((resolve) => {
    const overlay = document.getElementById("confirmOverlay");
    const okBtn = document.getElementById("confirmOk");
    const cancelBtn = document.getElementById("confirmCancel");
    document.getElementById("confirmMessage").textContent = message;
    overlay.hidden = false;
    const done = (result) => {
      overlay.hidden = true;
      okBtn.removeEventListener("click", onOk);
      cancelBtn.removeEventListener("click", onCancel);
      resolve(result);
    };
    const onOk = () => done(true);
    const onCancel = () => done(false);
    okBtn.addEventListener("click", onOk);
    cancelBtn.addEventListener("click", onCancel);
  });
}

async function requestQuit() {
  const ok = await confirmDialog("Would you like to quit?");
  if (!ok) return;
  // The native window's own close, bound by the Webview runtime — the same
  // call a raw Webview.Window app (no server) would use. Desktop.run also
  // exposes /__quit over HTTP, for closing the app from outside the window.
  window.__culebra_close__();
}
document.getElementById("quit").addEventListener("click", requestQuit);

// Opt-in hook the native runtime calls when the window frame's close button
// is clicked, instead of closing immediately. Without this, the frame closes
// the app straight away (see culebra_rt_webview.cc).
window.__culebra_before_close__ = requestQuit;

load();
