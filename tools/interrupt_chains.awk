# One line per catch-handler chain: FILE:LINE<TAB>TYPE<TAB>VERDICT.
#
# A chain is the run of handlers attached to one `try`. Handlers are contiguous
# in the source — `} catch (...) {` closes the previous one and opens the next,
# so that line ends a handler and starts another in the same chain.
#
# An interrupt is a `culebra::Interrupted`, which derives from nothing: only a
# handler naming it, or a catch-all, can take one. The handler that answers for
# an interrupt is the FIRST in the chain of either shape, and that is the only
# one this reports on.
#
# Verdicts:
#   EXPLICIT    names Interrupted, so it answers for one deliberately
#   RETHROWS    re-throws everything (`throw;`)
#   TERMINATES  ends the process, so nothing is swallowed
#   DOCUMENTED  carries an `// interrupt: <why an interrupt cannot arrive>` note
#   SWALLOWS    none of the above

function classify(body, raw, type,   b) {
  b = body
  if (type ~ /Interrupted/) return "EXPLICIT"
  if (b ~ /(^|[^_[:alnum:]])throw[[:space:]]*;/) return "RETHROWS"
  if (b ~ /std::(terminate|abort|_Exit)|(^|[^_[:alnum:]])abort[[:space:]]*\(/)
    return "TERMINATES"
  if (raw ~ /\/\/[[:space:]]*interrupt:/) return "DOCUMENTED"
  return "SWALLOWS"
}

function flush_chain(   i) {
  for (i = 1; i <= chain_n; i++) {
    if (chain_type[i] ~ /Interrupted|^\.\.\.$/) {
      printf "%s:%d\t%s\t%s\n", FILENAME, chain_line[i], chain_type[i],
             classify(chain_body[i], chain_raw[i], chain_type[i])
      break
    }
  }
  chain_n = 0
}

BEGIN { in_body = 0; chain_n = 0; prev_end = -1 }

FNR == 1 { flush_chain(); prev_end = -1; in_body = 0 }

{
  raw = $0
  line = $0
  sub(/\/\/.*$/, "", line)   # a comment must not supply `throw;`

  # `} catch (...) {` is balanced, so brace counting alone would run the whole
  # chain together as one handler — and then a chain whose first handler is a
  # type this does not report on would be skipped whole. Close the handler here
  # instead, at the `}` that ends its body, and let the branch below open the
  # next one in the same chain.
  if (in_body && depth == 1 &&
      line ~ /^[[:space:]]*\}[[:space:]]*catch[[:space:]]*\(/) {
    in_body = 0
    prev_end = FNR
  }

  if (!in_body && line ~ /catch[[:space:]]*\(/) {
    if (FNR != prev_end) flush_chain()
    rest = substr(line, index(line, "catch"))  # drop the `}` closing the last handler
    type = rest
    sub(/^catch[[:space:]]*\(/, "", type)
    sub(/\).*$/, "", type)
    sub(/^[[:space:]]*const[[:space:]]+/, "", type)
    sub(/[[:space:]]*&[[:space:]]*[[:alnum:]_]*[[:space:]]*$/, "", type)
    gsub(/^[[:space:]]+|[[:space:]]+$/, "", type)
    chain_n++
    chain_line[chain_n] = FNR
    chain_type[chain_n] = type
    chain_body[chain_n] = ""
    # The note may sit just above the handler, so the raw window starts there.
    chain_raw[chain_n] = pending_raw
    depth = 0
    in_body = 1
    line = rest        # count braces from the handler's own `{` onward
    seen_open = 0
  }

  if (in_body) {
    chain_body[chain_n] = chain_body[chain_n] " " line
    chain_raw[chain_n] = chain_raw[chain_n] " " raw
    n = gsub(/\{/, "{", line)
    m = gsub(/\}/, "}", line)
    if (n > 0) seen_open = 1
    depth += n - m
    if (seen_open && depth <= 0) {
      in_body = 0
      prev_end = FNR
    }
  }

  # Keep the last few raw lines so a note above the `try` is still in view.
  pending_raw = prev_raw2 " " prev_raw1 " " raw
  prev_raw2 = prev_raw1
  prev_raw1 = raw
}

END { flush_chain() }
