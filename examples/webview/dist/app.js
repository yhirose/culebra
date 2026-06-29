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
// Close the app over the HTTP bridge. Harmless (404, ignored) if the server
// didn't register /__quit.
document.getElementById("quit").addEventListener("click", () => {
  fetch("/__quit", { method: "POST" }).catch(() => {});
});
load();
