#!/usr/bin/env bash
# Long-width ratchet.
#
# culebra's Long is 64-bit, and `Value` carries it as int64_t. C++ `long` is
# 32-bit on Windows (LLP64) and, even where it is 64-bit, a DISTINCT type from
# int64_t on macOS — so a language value that passes through a `long` is either
# silently truncated or a runtime bad_any_cast, on platforms this repo's own
# developers do not build on. Linux cannot see any of it: there, `long` IS
# int64_t, so neither the compiler nor a local test can tell.
#
# This is the cheap half of the guard (the other is misc/check_llp64_width.sh
# in CI, which runs a real LLP64 compiler). It counts the FORMS that mean "a
# language value in a 32-bit-on-Windows type". Positions (line/column) and OS
# API types are not language values and are excluded — anything else must be
# int64_t. Lower a ceiling when you convert a site; never raise one without
# review.
set -euo pipefail
cd "$(dirname "$0")/.."

SRC=(include src)
fail=0

# All occurrences of a form, ignoring `long long` (a deliberate 64-bit
# spelling). One pipeline for counting AND listing, so the reported number and
# the shown offenders cannot come from different filters.
matches() { grep -rnE "$1" --include=*.h --include=*.cc "${SRC[@]}" 2>/dev/null \
            | grep -vE 'long long' || true; }

ratchet() { # name pattern ceiling hint  (same shape as check_rc_discipline.sh)
  local n; n=$(matches "$2" | grep -c . || true)
  if (( n > $3 )); then
    echo "long-width FAIL: $1 = $n (ceiling $3)" >&2
    echo "  $4" >&2
    matches "$2" | head -10 >&2
    fail=1
  fi
}

# A value read out of the language, put straight into a `long`. Every one of
# these truncates on Windows. Ceiling 0: there is no legitimate form.
ratchet "long <- to_long()"      '(^|[^A-Za-z0-9_])long [a-z_]+ *= *[^;]*to_long\(\)'      0 "Use int64_t."
ratchet "long <- get<int64_t>()" '(^|[^A-Za-z0-9_])long [a-z_]+ *= *[^;]*get<int64_t>\(\)' 0 "Use int64_t."
ratchet "long <- JitValue.data"  '(^|[^A-Za-z0-9_])long [a-z_]+ *= *[^;]*\.data([^A-Za-z0-9_]|$)'         0 "Use int64_t."
ratchet "get<long>"              'get<long>'                                0 "Payload is int64_t."
ratchet "Value(static_cast<long>" 'Value\(static_cast<long>'                0 "Cast to int64_t."
ratchet "hash<long>"             'std::hash<long>'                          0 "Use culebra::hash_long."
# A Float converted to the language's integer: same truncation, one step later.
ratchet "Float -> long"          'static_cast<long>\([^)]*(to_double|get<double>|float_to_double)' \
                                 0 "Cast to int64_t."

# Everything else that says static_cast<long>, minus the legitimate uses:
# positions (line / column / row as whole identifier segments — `_` counts as
# a boundary, so name_line is waived but lineno_limit and colors are not) and
# OS API types. No \b anywhere: BSD grep on macOS has no \b, and a pattern it
# cannot parse waives nothing, which failed the macOS gate at 220/64. The
# boundary groups consume a real character, so the whole cast is matched in
# one pattern instead of a zero-width lookaround.
POSCAST='static_cast<long>\(([^)]*[^A-Za-z0-9])?(line|column|col|row)([^A-Za-z0-9][^)]*)?\)?$'
OSAPI='(timeval|tm_gmtoff|os_timegm|st_uid|st_gid|pw_uid|gr_gid|perms::mask)'
residual_matches() {
  grep -rnoE "static_cast<long>\([^)]*\)" --include=*.h --include=*.cc \
       "${SRC[@]}" 2>/dev/null \
    | grep -viE "$POSCAST" | grep -viE "$OSAPI" || true
}
residual=$(residual_matches | grep -c . || true)
if (( residual > 64 )); then
  echo "long-width FAIL: non-position static_cast<long> = $residual (ceiling 64)" >&2
  echo "  A new one is a language value unless it is a bounded internal." >&2
  residual_matches | head -10 >&2
  fail=1
fi

if (( fail == 0 )); then
  echo "long-width OK (dangerous forms 0, residual casts $residual/64)"
fi
exit $fail
