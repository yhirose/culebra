; Tree-sitter highlight queries for Culebra (Zed). Mirrors the cul.vim / VSCode
; TextMate scopes against the minimal grammar in ../../tree-sitter-culebra.
; Later patterns win, so the call-name override comes after (identifier).

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

(string) @string
(regex) @string.regex
(number) @number

(type) @type
(identifier) @variable
(call name: (identifier) @function)

(operator) @operator
((punctuation) @punctuation.bracket
 (#match? @punctuation.bracket "^[()\\[\\]{}]$"))
((punctuation) @punctuation.delimiter
 (#match? @punctuation.delimiter "^[,.;]$"))
