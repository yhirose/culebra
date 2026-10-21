#!/usr/bin/env bash
# Object-facts gate: every data member of JitObject has a row in the
# object-facts table in include/conc/sendable.h (see the table for why), and
# the rows that say "carried" name exactly the members kCarriedFacts carries.
# A new member in neither fails here, until someone decides which.
set -euo pipefail
cd "$(dirname "$0")/../.."

value=include/rt/value.inc.h
table=include/conc/sendable.h

# The declarations at the struct's own level and inside its anonymous unions,
# never a local of a member function's body.
members=$(awk '
  /^struct JitObject \{/ { in_struct = 1; depth = 1; next }
  !in_struct { next }
  {
    line = $0
    sub(/\/\/.*/, "", line)
    opens = gsub(/\{/, "{", line)
    closes = gsub(/\}/, "}", line)
    at_member_level = (depth == 1) || (depth == 2 && in_union)
    if (at_member_level && opens == 0 && closes == 0 &&
        line ~ /^ +[A-Za-z_][A-Za-z0-9_:<>,* ]* \*?[a-z_]+( = [^;]*)?; *$/ &&
        line !~ /^ +(static|using|return|friend|struct|typedef|enum) /) {
      name = line
      sub(/( = [^;]*)?; *$/, "", name)
      sub(/.*[ *]/, "", name)
      print name
    }
    if (depth == 1 && line ~ /^ +union *\{/) in_union = 1
    depth += opens - closes
    if (in_union && depth == 1) in_union = 0
    if (depth == 0) exit
  }' "$value" | sort -u)
rows=$(sed -n '/object-facts: begin/,/object-facts: end/p' "$table")
listed=$(printf '%s\n' "$rows" | grep -oE '^//   [a-z_]+ ' | awk '{ print $2 }' | sort -u)
carried_rows=$(printf '%s\n' "$rows" | grep -E '^//   [a-z_]+ +carried' \
               | awk '{ print $2 }' | sort -u)
carried_code=$(sed -n '/kCarriedFacts\[\] = {/,/};/p' "$table" \
               | grep -oE '&JitObject::[a-z_]+' | sed 's/&JitObject:://' | sort -u)

n_members=$(printf '%s\n' "$members" | grep -c . || true)
n_listed=$(printf '%s\n' "$listed" | grep -c . || true)
n_carried=$(printf '%s\n' "$carried_code" | grep -c . || true)
if [[ "$n_members" -lt 30 || "$n_listed" -lt 30 || "$n_carried" -lt 4 ]]; then
  echo "object-facts: read $n_members JitObject members, $n_listed table rows" \
       "and $n_carried carried facts -- struct JitObject, the table or" \
       "kCarriedFacts in $table moved and this gate stopped reading it" >&2
  exit 1
fi

fail=0
missing=$(comm -23 <(printf '%s\n' "$members") <(printf '%s\n' "$listed"))
stale=$(comm -13 <(printf '%s\n' "$members") <(printf '%s\n' "$listed"))
# has_drop is carried beside kCarriedFacts: restoring it registers a drop.
unbacked=$(comm -23 <(printf '%s\n' "$carried_rows" | grep -vx has_drop) \
                    <(printf '%s\n' "$carried_code"))
unlisted=$(comm -13 <(printf '%s\n' "$carried_rows") <(printf '%s\n' "$carried_code"))
if [[ -n "$missing" ]]; then
  echo "object-facts: JitObject members the table in $table does not account" \
       "for (carry it, or say why it stays behind):" $missing >&2
  fail=1
fi
if [[ -n "$stale" ]]; then
  echo "object-facts: table rows naming no JitObject member:" $stale >&2
  fail=1
fi
if [[ -n "$unbacked" || -n "$unlisted" ]]; then
  echo "object-facts: the table's \"carried\" rows and kCarriedFacts disagree:" \
       ${unbacked:+rows only:} $unbacked ${unlisted:+kCarriedFacts only:} $unlisted >&2
  fail=1
fi
[[ $fail -eq 0 ]] || exit 1

echo "object-facts OK ($n_members JitObject members, each carried, restored or left behind)"
