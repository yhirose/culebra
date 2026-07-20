// CodeMirror 6 wiring for the playground editor. Loaded from esm.sh (CDN,
// no local vendoring/build step) — pinned versions across every import so
// the browser resolves one shared @codemirror/state|view module instance
// (mismatched versions would otherwise create duplicate Facet/Extension
// classes and break composition).
import { EditorView, basicSetup } from "https://esm.sh/codemirror@6.0.1";
import { keymap } from "https://esm.sh/@codemirror/view@6.23.0";
import { indentWithTab } from "https://esm.sh/@codemirror/commands@6.3.0";
import { syntaxHighlighting } from "https://esm.sh/@codemirror/language@6.12.4";
import { culebraLanguage, culebraHighlightStyle } from "./culebra-lang.js";

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
});

export function createEditor(parent, doc) {
  const view = new EditorView({
    doc,
    extensions: [
      basicSetup,
      keymap.of([indentWithTab]),
      culebraLanguage,
      syntaxHighlighting(culebraHighlightStyle),
      theme,
    ],
    parent,
  });
  return {
    getValue: () => view.state.doc.toString(),
    setValue(text) {
      view.dispatch({
        changes: { from: 0, to: view.state.doc.length, insert: text },
      });
    },
    focus: () => view.focus(),
  };
}
