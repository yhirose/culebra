// Minimal Tree-sitter grammar for Culebra (.cul) — lexical highlighting only.
//
// Zed requires a Tree-sitter grammar for syntax highlighting (it cannot use the
// TextMate grammar that VSCode uses). This grammar deliberately does NOT model
// Culebra's structure: it parses a file as a flat stream of tokens (comments,
// strings, numbers, keywords, types, calls, identifiers, operators) so that
// `highlights.scm` can colour them. That keeps it small and resilient — it is a
// lexer, not the language's real parser (the PEG in include/parser.h is that).
//
// The keyword rules mirror misc/keyword-map.txt; keep them in step with the
// language (the cul.vim / VSCode keyword lists are the other mirrors).

module.exports = grammar({
  name: "culebra",

  // Identifier is the "word" token so Tree-sitter does keyword extraction
  // (e.g. `forever` is not split into the keyword `for` + `ever`).
  word: ($) => $.identifier,

  extras: ($) => [/\s/, $.line_comment, $.block_comment],

  rules: {
    source_file: ($) => repeat($._token),

    _token: ($) =>
      choice(
        $.kw_function,
        $.kw_class,
        $.kw_conditional,
        $.kw_repeat,
        $.kw_statement,
        $.kw_include,
        $.kw_debugger,
        $.storage,
        $.boolean,
        $.nil,
        $.self,
        $.regex,
        $.string,
        $.number,
        $.call,
        $.type,
        $.identifier,
        $.operator,
        $.punctuation,
      ),

    // --- comments (in `extras`, so they may appear anywhere) ---
    line_comment: ($) => token(seq(choice("//", "#"), /[^\n]*/)),
    block_comment: ($) => token(seq("/*", /[^*]*\*+([^/*][^*]*\*+)*/, "/")),

    // --- keywords (mirror misc/keyword-map.txt) ---
    kw_function: ($) => "fn",
    kw_class: ($) => choice("class", "trait", "enum"),
    kw_conditional: ($) => choice("if", "else", "match", "cond"),
    kw_repeat: ($) => choice("while", "for", "in"),
    kw_statement: ($) =>
      choice(
        "return",
        "break",
        "continue",
        "throw",
        "try",
        "catch",
        "defer",
        "yield",
      ),
    kw_include: ($) => choice("import", "export", "from"),
    kw_debugger: ($) => "debugger",
    storage: ($) => choice("let", "mut", "static"),
    boolean: ($) => choice("true", "false"),
    nil: ($) => "nil",
    self: ($) => choice("self", "this", "__ARGS__"),

    // --- literals ---
    // Float before int is handled by `number`'s alternation order + length.
    number: ($) =>
      token(
        choice(
          /\d+\.\d+([eE][-+]?\d+)?/,
          /\d+[eE][-+]?\d+/,
          /\d+/,
        ),
      ),

    // Single-quoted (raw), double-quoted (with escapes). Interpolation is not
    // modelled (the whole literal is one token) — good enough for colouring.
    string: ($) =>
      token(
        choice(
          seq('"', repeat(choice(/[^"\\]/, /\\./)), '"'),
          seq("'", repeat(choice(/[^'\\]/, /\\./)), "'"),
        ),
      ),

    // Regex literals: re'...' / re"..." / re`...` with trailing [ims] flags.
    regex: ($) =>
      token(
        seq(
          "re",
          choice(
            seq("'", repeat(choice(/[^'\\]/, /\\./)), "'"),
            seq('"', repeat(choice(/[^"\\]/, /\\./)), '"'),
            seq("`", repeat(choice(/[^`\\]/, /\\./)), "`"),
          ),
          /[ims]*/,
        ),
      ),

    // --- identifiers / types / calls ---
    // Lowercase-leading identifier is the `word`; keywords are a subset.
    identifier: ($) => /[a-z_][A-Za-z0-9_]*/,
    // Capitalized: built-in types, stdlib namespaces, user classes.
    type: ($) => /[A-Z][A-Za-z0-9_]*/,
    // A name immediately followed by `(` is a call site.
    call: ($) =>
      prec(
        1,
        seq(field("name", choice($.identifier, $.type)), token.immediate("(")),
      ),

    operator: ($) =>
      token(
        choice(
          "&&", "||", "??", "**", "=>", "->", "...", "..=", "..",
          "==", "!=", "<=", ">=", "+=", "-=", "*=", "/=", "%=",
          "&=", "|=", "^=", "@=",
          "+", "-", "*", "/", "%", "@", "!", "=", "<", ">", "^", "|", "&",
          "?", ":",
        ),
      ),

    punctuation: ($) => choice("(", ")", "[", "]", "{", "}", ",", ".", ";"),
  },
});
