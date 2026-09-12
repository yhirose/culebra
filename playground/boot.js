// What the root element has to say before the body is parsed, so the page is
// never painted one way and then corrected. A file rather than an inline
// <script> so the page's Content-Security-Policy can refuse inline script
// outright: a static site cannot use a nonce (the same value would ship to
// everyone), and a hash would have to be kept in step with this by hand.
//
// Loaded without defer or async, which is what keeps "before the body" true.

(function () {
  var search = new URLSearchParams(location.search);

  // ?embed= says how much of the page a hosting iframe wants: `editor` drops
  // the chrome that page already provides and keeps the editor and the
  // controls, `output` leaves the output pane alone (which pane is `?view=`).
  // `1` is the older spelling of `editor`. See the styles.css "embed modes"
  // block.
  var embed = search.get("embed");
  if (embed === "1" || embed === "editor") document.documentElement.classList.add("embed");
  if (embed === "output") document.documentElement.classList.add("embed-output");

  // ?split= puts the output under the source rather than beside it, for a
  // host that has the page's full width but only a few hundred pixels of it.
  if (search.get("split") === "top") document.documentElement.classList.add("split-top");

  // The theme the visitor chose on the site around this page.
  try {
    var t = localStorage.getItem("culebra-theme");
    if (t) document.documentElement.setAttribute("data-theme", t);
  } catch (e) {}
})();
