; Tree-sitter highlight queries for Culebra (Zed). Mirrors the cul.vim / VSCode
; TextMate scopes against the grammar in ../../tree-sitter-culebra.
;
; Precedence: a smaller (deeper) node wins for its own range, so an identifier
; inside a string interpolation is coloured as code even though the whole string
; node is @string. For two patterns on the same node, the later one wins (hence
; the call-name override comes after (identifier)).

(line_comment) @comment
(block_comment) @comment

(kw_function) @keyword
(kw_class) @keyword
(kw_conditional) @keyword
(kw_repeat) @keyword
(kw_statement) @keyword
(kw_include) @keyword
(kw_debugger) @keyword
(storage) @keyword

(boolean) @boolean
(nil) @constant.builtin
(self) @variable.special

; Strings — the parts outside interpolations. The interpolation's own tokens
; (identifiers, numbers, ...) are deeper nodes and keep their code colours.
(string) @string
(string_content) @string
(raw_content) @string
(triple_content) @string
(escape) @string.escape
(regex) @string.regex
(interpolation "{" @punctuation.special)
(interpolation "}" @punctuation.special)

(number) @number

(type) @type
(identifier) @variable
(call name: (identifier) @function)

(operator) @operator
(brace_block "{" @punctuation.bracket)
(brace_block "}" @punctuation.bracket)
((punctuation) @punctuation.bracket
 (#match? @punctuation.bracket "^[()\\[\\]]$"))
((punctuation) @punctuation.delimiter
 (#match? @punctuation.delimiter "^[,.;]$"))
