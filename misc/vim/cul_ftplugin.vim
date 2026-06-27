" Vim ftplugin for Culebra (.cul): wire `culebra fmt` in as the formatter.
" Installed as ftplugin/cul.vim by install.sh.

if exists("b:did_ftplugin")
  finish
endif
let b:did_ftplugin = 1

" `gq` (and `gqq`, `gqap`, ...) reformats through `culebra fmt`, reading the
" buffer on stdin and writing the canonical form to stdout.
setlocal formatprg=culebra\ fmt\ -

" Format-on-save: uncomment to reformat the whole buffer before each write.
" It preserves the cursor position and only rewrites when the file parses.
"
" augroup CulebraFmtOnSave
"   autocmd! * <buffer>
"   autocmd BufWritePre <buffer> call s:CulebraFormat()
" augroup END
"
" function! s:CulebraFormat() abort
"   let l:view = winsaveview()
"   silent! %!culebra fmt -
"   if v:shell_error != 0
"     silent! undo
"   endif
"   call winrestview(l:view)
" endfunction
