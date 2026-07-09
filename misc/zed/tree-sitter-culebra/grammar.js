// Minimal Tree-sitter grammar for Culebra (.cul) — lexical highlighting.
//
// Zed requires a Tree-sitter grammar for syntax highlighting (it cannot use the
// TextMate grammar that VSCode uses). This grammar does NOT model Culebra's
// structure: it parses a file as a stream of tokens (comments, strings, regex
// literals, numbers, keywords, types, calls, identifiers, operators) so that
// `highlights.scm` can colour them. It is a lexer, not the language's real
// parser (the PEG in include/parser.h is that).
//
// Two constructs need real structure, so they are rules rather than opaque
// tokens: string interpolation (`"... {expr} ..."`) and triple-quoted strings
// (`"""..."""`). Interpolations are balanced in pure grammar (braces only enter
// the token stream as a matched `brace_block`, so a lone `}` is unambiguously a
// closer). Triple-string content needs `"""` lookahead, which the regex subset
// can't express, so it comes from a tiny external scanner (src/scanner.c).
//
// The keyword rules mirror misc/keyword-map.txt; keep them in step with the
// language (the culebra.vim / VSCode keyword lists are the other mirrors).

// The shared code-token set, used both at top level and inside an
// interpolation. Excludes a bare `{`/`}` — those only enter as a matched
// `brace_block`, which keeps interpolation/brace_block closers unambiguous.
function codeTokens($) {
  return [
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
  ];
}

module.exports = grammar({
  name: "culebra",

  word: ($) => $.identifier,
  extras: ($) => [/\s/, $.line_comment, $.block_comment],
  externals: ($) => [$.triple_content],

  rules: {
    source_file: ($) => repeat($._item),

    _item: ($) => choice(...codeTokens($), $.brace_block),
    // A balanced `{ ... }` — the only way a brace enters the token stream.
    brace_block: ($) => seq("{", repeat($._item), "}"),

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

    // --- numbers ---
    number: ($) =>
      token(choice(/\d+\.\d+([eE][-+]?\d+)?/, /\d+[eE][-+]?\d+/, /\d+/)),

    // --- strings ---
    // Double-quoted strings interpolate `{expr}`; single-quoted are raw; triple
    // (`"""`) also interpolate and use the external scanner for their content.
    string: ($) =>
      choice(
        seq('"', repeat(choice($.string_content, $.escape, $.interpolation)), '"'),
        seq("'", repeat(choice($.raw_content, $.escape)), "'"),
        $.triple_string,
      ),
    triple_string: ($) =>
      seq(
        '"""',
        repeat(choice($.triple_content, $.escape, $.interpolation)),
        '"""',
      ),
    // A run of ordinary characters (a literal `}` is fine; `{` starts an
    // interpolation, `\` an escape, `"` ends the string).
    string_content: ($) => token.immediate(prec(1, /[^"\\{]+/)),
    raw_content: ($) => token.immediate(prec(1, /[^'\\]+/)),
    escape: ($) => token.immediate(/\\./),
    // `{ ... }` inside a string. The opening brace must be immediate (no
    // intervening whitespace) so it pairs with the string, not a block.
    interpolation: ($) => seq(token.immediate("{"), repeat($._item), "}"),

    // --- regex literals: re'...' / re"..." / re`...` with [ims] flags ---
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
    identifier: ($) => /[a-z_][A-Za-z0-9_]*/,
    type: ($) => /[A-Z][A-Za-z0-9_]*/,
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

    // Brackets and delimiters — braces are handled by brace_block.
    punctuation: ($) => choice("(", ")", "[", "]", ",", ".", ";"),
  },
});
