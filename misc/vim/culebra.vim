" Vim syntax file for Culebra (.cul)
" Tracks the grammar in include/parser.h.

if exists("b:current_syntax")
  finish
endif

" Numbers — float patterns first so the integer rule does not steal them.
syn match   culFloat        "\<\d\+\.\d\+\%([eE][-+]\?\d\+\)\?\>"
syn match   culFloat        "\<\d\+[eE][-+]\?\d\+\>"
syn match   culDecNumber    "\<\d\+\>"

" Strings
syn region  culStringS      start=+'+ skip=+\\\\\|\\'+ end=+'\|$+
syn region  culStringD      start=+"+ skip=+\\\\\|\\"+ end=+"\|$+ contains=culInterp
syn region  culInterp       matchgroup=culInterpDelim
                            \ start=+{+ end=+}+ contained contains=TOP

" Regex literals: re'...' / re"..." / re`...` with trailing [ims] flags.
" The 're' prefix must sit at a word boundary and be immediately followed by a
" quote (a bare 're' stays an ordinary identifier). Each region starts at 're'
" (an earlier column than the bare-quote string regions above) so it wins the
" overlap and the body highlights as a regex, not a string. The body is raw
" apart from the dollar-brace interpolation; a backslash-escape (incl. an
" escaped dollar, which suppresses interpolation) is flagged. culRegexEscape
" never consumes a quote, so it cannot swallow a closing delimiter.
syn match   culRegexEscape  +\\[^'"`]+ contained
syn region  culRegexInterp  matchgroup=culInterpDelim start=+\${+ end=+}+ contained contains=TOP
syn region  culRegex        matchgroup=culRegexDelim start=+\<re'+ end=+'[ims]*\|$+ contains=culRegexEscape,culRegexInterp
syn region  culRegex        matchgroup=culRegexDelim start=+\<re"+ end=+"[ims]*\|$+ contains=culRegexEscape,culRegexInterp
syn region  culRegex        matchgroup=culRegexDelim start=+\<re`+ end=+`[ims]*\|$+ contains=culRegexEscape,culRegexInterp

" Operators (multi-char first so prefixes don't shadow them)
syn match   culOperator     "&&\|||\|??\|\*\*\|=>\|->\|\.\.\.\|\.\.=\?"
syn match   culOperator     "[-+*/%@!=<>^|&]=\?"

" Comments — defined after operators so '//' and '/*' win the '/' over culOperator.
" (When two matches start at the same column, Vim gives the later one priority.)
syn keyword culCommentTodo  TODO FIXME XXX TBD contained
syn match   culLineComment  "\%(\/\/\|#\).*"      contains=culCommentTodo,@Spell
syn region  culComment      start="/\*" end="\*/" contains=culCommentTodo,@Spell

" Keywords and built-in names, auto-generated from misc/keyword-map.txt by
" `just sync-grammar` (non-PEG identifiers are below). culBuiltin holds the
" stdlib namespaces and type-annotation names; a `syn keyword` outranks the
" culUserType match below, so those keep the built-in colour.
" === BEGIN AUTO-KEYWORDS (from misc/keyword-map.txt via `just sync-grammar`) ===
syn keyword culFunction    fn
syn keyword culClass       class trait enum
syn keyword culConditional if unless else match cond
syn keyword culRepeat      while for in by
syn keyword culStatement   return break continue nobreak throw try catch defer yield
syn keyword culEffect      effect perform handle with
syn keyword culInclude     import export from
syn keyword culDebugger    debugger
syn keyword culBoolean     true false
syn keyword culConstant    nil
syn keyword culStorage     let mut static get
syn keyword culBuiltin     Nil Bool Long Float String Array Object Function Any
syn keyword culBuiltin     Math IO FS File Embed Time Random Sys Tensor JSON Args Proc Path
syn keyword culBuiltin     Isolate Channel Parallel Signal SharedBuffer Shared GC Regex Http
syn keyword culBuiltin     Encoding Compress Hash CSV Env UUID Term Log TOML SQLite
syn keyword culBuiltin     Canvas Scene Net Desktop Webview Vector2 Vector3 Deque PriorityQueue
syn keyword culBuiltin     Peg
" === END AUTO-KEYWORDS ===

" Conventional identifiers that aren't grammar keywords.
syn keyword culSelf         self __ARGS__

" Capitalized identifiers the culBuiltin list did not claim: names declared in
" the program itself (classes, traits, enums and their variants). Coloured as
" Identifier rather than Type so `Anim` does not read as a name the language
" already knows, the way `Canvas` and `Long` do.
syn match   culUserType     "\<[A-Z][A-Za-z0-9_]*\>"

" Function-call sites
syn match   culFuncCall     "\<\h\w*\ze("

hi def link culLineComment   Comment
hi def link culComment       Comment
hi def link culCommentTodo   Todo
hi def link culFloat         Float
hi def link culDecNumber     Number
hi def link culStringS       String
hi def link culStringD       String
hi def link culInterpDelim   Special
hi def link culRegex         String
hi def link culRegexDelim    Special
hi def link culRegexEscape   SpecialChar
hi def link culOperator      Operator
hi def link culFunction      Type
hi def link culClass         Structure
hi def link culConditional   Conditional
hi def link culRepeat        Repeat
hi def link culStatement     Statement
hi def link culEffect        Keyword
hi def link culInclude       Include
hi def link culDebugger      Debug
hi def link culBoolean       Boolean
hi def link culConstant      Constant
hi def link culSelf          Constant
hi def link culStorage       StorageClass
hi def link culBuiltin       Type
hi def link culUserType      Identifier
hi def link culFuncCall      Function

let b:current_syntax = "culebra"
