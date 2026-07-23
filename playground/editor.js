// CodeMirror 6 wiring for the playground editor. Loaded from esm.sh (CDN, no
// local vendoring/build step). esm.sh dedupes @codemirror/state|view across
// packages ONLY when the versions agree with what `codemirror@6.0.1` resolves
// transitively — that is @codemirror/state@6.7.1. Importing StateField from any
// OTHER state version loads a second instance and breaks instanceof-based
// extension resolution ("Unrecognized extension value"). So the direct
// @codemirror/state import below must stay pinned to 6.7.1.
import { EditorView, basicSetup } from "https://esm.sh/codemirror@6.0.1";
import { keymap, Decoration } from "https://esm.sh/@codemirror/view@6.23.0";
import { StateField, StateEffect } from "https://esm.sh/@codemirror/state@6.7.1";
import { indentWithTab } from "https://esm.sh/@codemirror/commands@6.3.0";
import { syntaxHighlighting } from "https://esm.sh/@codemirror/language@6.12.4";
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

export function createEditor(parent, doc) {
  const view = new EditorView({
    doc,
    extensions: [
      basicSetup,
      keymap.of([indentWithTab]),
      culebraLanguage,
      syntaxHighlighting(culebraHighlightStyle),
      errorLineField,
      theme,
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
