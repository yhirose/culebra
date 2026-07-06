" Vim ftplugin for Culebra (.cul): format via `culebra fmt`.
" Installed as ftplugin/cul.vim by install.sh.
"
" `culebra fmt` is a whole-file formatter (like gofmt / rustfmt), so this does
" NOT wire it into `gq` / 'formatprg': filtering a partial range — or any text
" that fails to parse — through it would replace those lines with the empty
" output and lose them. Instead it formats the whole buffer through a guard
" that only rewrites it on success (errors leave the buffer untouched and show
" the message), preserving the cursor — the same approach vim-go and rust.vim
" take for their whole-file formatters.

if exists('b:did_ftplugin')
  finish
endif
let b:did_ftplugin = 1

function! s:CulebraFormat() abort
  if !&modifiable
    return
  endif
  let l:view = winsaveview()
  let l:src = join(getline(1, '$'), "\n") . "\n"
  let l:out = system('culebra fmt - 2>&1', l:src)
  if v:shell_error != 0
    " Parse / safety error (or `culebra` not on $PATH): leave the buffer as is.
    echohl WarningMsg
    echomsg 'culebra fmt: ' . substitute(l:out, '\n', ' ', 'g')
    echohl None
    return
  endif
  let l:lines = split(l:out, "\n", 1)
  if !empty(l:lines) && l:lines[-1] ==# ''
    call remove(l:lines, -1)        " drop the element after the final newline
  endif
  if l:lines ==# getline(1, '$')
    return                          " already formatted — no edit, no undo entry
  endif
  call setline(1, l:lines)
  if line('$') > len(l:lines)
    call deletebufline('%', len(l:lines) + 1, '$')
  endif
  call winrestview(l:view)
endfunction

" :CulebraFmt — reformat the whole buffer.
command! -buffer CulebraFmt call <SID>CulebraFormat()

" Format on save: `let g:culebra_fmt_autosave = 1` in your vimrc to enable
" (off by default, mirroring g:go_fmt_autosave / g:rustfmt_autosave).
if get(g:, 'culebra_fmt_autosave', 0)
  augroup CulebraFmtOnSave
    autocmd! * <buffer>
    autocmd BufWritePre <buffer> call <SID>CulebraFormat()
  augroup END
endif

" Suggested mapping (add to your vimrc):
"   autocmd FileType cul nnoremap <buffer> <silent> <leader>f :CulebraFmt<CR>
