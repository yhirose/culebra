// CodeMirror 6 wiring for the playground editor. Loaded from esm.sh (CDN, no
// local vendoring/build step). esm.sh dedupes a package across imports ONLY
// when the version strings agree byte-for-byte; `codemirror@6.0.1` (which
// carries basicSetup) pulls @codemirror/state|view|commands|language via
// floating `^6.0.0` ranges that esm.sh re-resolves on every request. A pinned
// exact version here can drift from wherever that floating range currently
// lands, loading a second module instance and breaking instanceof-based
// extension resolution ("Unrecognized extension value"). So every direct
// import below must mirror codemirror@6.0.1's own ranges, not a fixed number.
import { EditorView, basicSetup } from "https://esm.sh/codemirror@6.0.1";
import { keymap, Decoration } from "https://esm.sh/@codemirror/view@^6.0.0";
import { StateField, StateEffect } from "https://esm.sh/@codemirror/state@^6.0.0";
import { indentWithTab } from "https://esm.sh/@codemirror/commands@^6.0.0";
import { syntaxHighlighting } from "https://esm.sh/@codemirror/language@^6.0.0";
import { culebraLanguage, culebraHighlightStyle } from "./culebra-lang.js";

// Error-line highlight: a failed run carries `at LINE:COL` in its message (see
// app.js), and we mark that line — the jump-to-the-error affordance the Rust/Go
// playgrounds have. A StateField holds at most one line decoration; a
// `setErrorLine` effect sets or clears it, and any edit clears it (a stale
// marker on now-changed code is worse than none).
const setErrorLine = StateEffect.define();
const errorLineDeco = Decoration.line({ class: "cm-errorLine" });
const errorLineField = StateField.define({
  create: () => Decoration.none,
  update(deco, tr) {
    for (const e of tr.effects) {
      if (e.is(setErrorLine)) {
        if (e.value == null) return Decoration.none;
        const n = Math.max(1, Math.min(e.value, tr.state.doc.lines));
        return Decoration.set([errorLineDeco.range(tr.state.doc.line(n).from)]);
      }
    }
    return tr.docChanged ? Decoration.none : deco;
  },
  provide: (f) => EditorView.decorations.from(f),
});

// Colors reference brand.css custom properties directly, so this one theme
// works for both light and dark (see culebra-lang.js's HighlightStyle too).
const theme = EditorView.theme({
  "&": {
    height: "100%",
    fontSize: "0.92rem",
    backgroundColor: "var(--code-bg)",
    color: "var(--text)",
  },
  ".cm-content": { fontFamily: "var(--font-mono)", padding: "1rem 1.2rem" },
  ".cm-scroller": { fontFamily: "var(--font-mono)", overflow: "auto" },
  ".cm-gutters": {
    backgroundColor: "var(--code-bg)",
    color: "var(--muted)",
    border: "none",
  },
  ".cm-activeLine": { backgroundColor: "transparent" },
  ".cm-activeLineGutter": { backgroundColor: "transparent" },
  "&.cm-focused": { outline: "none" },
  ".cm-errorLine": {
    backgroundColor: "rgba(220, 60, 60, 0.13)",
    boxShadow: "inset 3px 0 0 #dc3c3c",
  },
});

export function createEditor(parent, doc, { onChange } = {}) {
  const view = new EditorView({
    doc,
    extensions: [
      basicSetup,
      keymap.of([indentWithTab]),
      culebraLanguage,
      syntaxHighlighting(culebraHighlightStyle),
      errorLineField,
      theme,
      // Fires on every edit AND on setValue() below (an example load, or the
      // draft restore itself) — app.js wants "whatever the editor shows now"
      // either way, not just hand-typed changes. No document text is built
      // here; a debounced caller reads it lazily via getValue() when it's
      // actually ready to use it, not on every keystroke.
      EditorView.updateListener.of((update) => {
        if (onChange && update.docChanged) onChange();
      }),
    ],
    parent,
  });
  return {
    getValue: () => view.state.doc.toString(),
    setValue(text) {
      view.dispatch({
        changes: { from: 0, to: view.state.doc.length, insert: text },
        effects: setErrorLine.of(null),
      });
    },
    focus: () => view.focus(),
    // Highlight (and scroll to) a 1-based line reported by a failed run.
    setError(line) {
      const n = Math.max(1, Math.min(line, view.state.doc.lines));
      const pos = view.state.doc.line(n).from;
      view.dispatch({
        effects: [setErrorLine.of(line), EditorView.scrollIntoView(pos, { y: "center" })],
      });
    },
    clearError() {
      view.dispatch({ effects: setErrorLine.of(null) });
    },
  };
}
