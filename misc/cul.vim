" Vim syntax file for Culebra (.cul)
" Tracks the grammar in include/parser.h.

if exists("b:current_syntax")
  finish
endif

" Comments
syn keyword culCommentTodo  TODO FIXME XXX TBD contained
syn match   culLineComment  "\%(\/\/\|#\).*"      contains=culCommentTodo,@Spell
syn region  culComment      start="/\*" end="\*/" contains=culCommentTodo,@Spell

" Numbers — float patterns first so the integer rule does not steal them.
syn match   culFloat        "\<\d\+\.\d\+\%([eE][-+]\?\d\+\)\?\>"
syn match   culFloat        "\<\d\+[eE][-+]\?\d\+\>"
syn match   culDecNumber    "\<\d\+\>"

" Strings
syn region  culStringS      start=+'+ skip=+\\\\\|\\'+ end=+'\|$+
syn region  culStringD      start=+"+ skip=+\\\\\|\\"+ end=+"\|$+ contains=culInterp
syn region  culInterp       matchgroup=culInterpDelim
                            \ start=+{+ end=+}+ contained contains=TOP

" Operators (multi-char first so prefixes don't shadow them)
syn match   culOperator     "&&\|||\|??\|\*\*\|=>\|->\|\.\.\.\|\.\.=\?"
syn match   culOperator     "[-+*/%@!=<>^|&]=\?"

" Keywords (PEG-derived keywords are auto-generated from misc/keyword-map.txt
" by `just sync-grammar`; non-PEG identifiers are below).
" === BEGIN AUTO-KEYWORDS (from misc/culebra.peg via `just sync-grammar`) ===
syn keyword culFunction    fn
syn keyword culClass       class trait enum
syn keyword culConditional if else match
syn keyword culRepeat      while for in
syn keyword culStatement   return break continue throw try catch defer yield
syn keyword culInclude     import export from
syn keyword culDebugger    debugger
syn keyword culBoolean     true false
syn keyword culConstant    nil
syn keyword culStorage     let mut static
" === END AUTO-KEYWORDS ===

" Conventional identifiers that aren't grammar keywords.
syn keyword culSelf         self this __ARGS__

" Capitalized identifiers — built-in types (Long/Float/String/Bool/...),
" stdlib namespaces (Math/IO/Random), user class names.
syn match   culType         "\<[A-Z][A-Za-z0-9_]*\>"

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
hi def link culOperator      Operator
hi def link culFunction      Type
hi def link culClass         Structure
hi def link culConditional   Conditional
hi def link culRepeat        Repeat
hi def link culStatement     Statement
hi def link culInclude       Include
hi def link culDebugger      Debug
hi def link culBoolean       Boolean
hi def link culConstant      Constant
hi def link culSelf          Constant
hi def link culStorage       StorageClass
hi def link culType          Type
hi def link culFuncCall      Function

let b:current_syntax = "cul"
