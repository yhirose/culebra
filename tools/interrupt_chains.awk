# One line per catch-handler chain: FILE:LINE<TAB>TYPE<TAB>VERDICT.
#
# A chain is the run of handlers attached to one `try`. Handlers are contiguous
# in the source — `} catch (...) {` closes the previous one — so a catch that
# opens on the line where the previous handler's body closed is in the same
# chain.
#
# CulebraError derives from std::runtime_error, so the handler that answers for
# an interrupt is the FIRST in the chain whose type can catch one. That is the
# only handler this reports on.
#
# Verdicts:
#   CHECKED     asks is_interrupt() and re-throws it
#   RETHROWS    re-throws everything (`throw;`)
#   TERMINATES  ends the process, so nothing is swallowed
#   DOCUMENTED  carries an `// interrupt: <why an interrupt cannot arrive>` note
#   SWALLOWS    none of the above

function classify(body, raw,   b) {
  b = body
  if (b ~ /is_interrupt/) return "CHECKED"
  if (b ~ /(^|[^_[:alnum:]])throw[[:space:]]*;/) return "RETHROWS"
  if (b ~ /std::(terminate|abort|_Exit)|(^|[^_[:alnum:]])abort[[:space:]]*\(/)
    return "TERMINATES"
  if (raw ~ /\/\/[[:space:]]*interrupt:/) return "DOCUMENTED"
  return "SWALLOWS"
}

function flush_chain(   i) {
  for (i = 1; i <= chain_n; i++) {
    if (chain_type[i] ~ /CulebraError|std::runtime_error|std::exception|^\.\.\.$/) {
      printf "%s:%d\t%s\t%s\n", FILENAME, chain_line[i], chain_type[i],
             classify(chain_body[i], chain_raw[i])
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
    # The note may sit just above the `try`, so the raw window starts there.
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
