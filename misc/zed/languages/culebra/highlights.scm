; Tree-sitter highlight queries for Culebra (Zed). Mirrors the culebra.vim / VSCode
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
(kw_effect) @keyword
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

; A capitalized name is its own `type` token in the grammar, so the split below
; is only builtin vs user-declared: the queries between the markers re-capture
; the stdlib namespaces and type-annotation names as @type.builtin, leaving a
; class/enum declared in the program itself on the plain @type colour.
(type) @type
; === BEGIN AUTO-BUILTINS (from misc/keyword-map.txt via `just sync-grammar`) ===
((type) @type.builtin
 (#match? @type.builtin "^(Nil|Bool|Long|Float|String|Array|Object|Function|Any)$"))
((type) @type.builtin
 (#match? @type.builtin "^(Math|IO|FS|File|Embed|Time|Random|Sys|Tensor|JSON|Args|Proc|Path)$"))
((type) @type.builtin
 (#match? @type.builtin "^(Isolate|Channel|Parallel|Signal|SharedBuffer|Shared|GC|Regex|Http)$"))
((type) @type.builtin
 (#match? @type.builtin "^(Encoding|Compress|Hash|CSV|Env|UUID|Term|Log|TOML|SQLite)$"))
((type) @type.builtin
 (#match? @type.builtin "^(Canvas|Scene|Net|Desktop|Webview|Vector2|Vector3|Deque|PriorityQueue)$"))
((type) @type.builtin
 (#match? @type.builtin "^(Peg)$"))
; === END AUTO-BUILTINS ===

(identifier) @variable
(call name: (identifier) @function)

(operator) @operator
(brace_block "{" @punctuation.bracket)
(brace_block "}" @punctuation.bracket)
((punctuation) @punctuation.bracket
 (#match? @punctuation.bracket "^[()\\[\\]]$"))
((punctuation) @punctuation.delimiter
 (#match? @punctuation.delimiter "^[,.;]$"))
