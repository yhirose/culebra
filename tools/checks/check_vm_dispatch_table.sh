#!/usr/bin/env bash
# Dispatch-table order gate.
#
# The executor dispatches through `kLabels`, an array of label addresses
# indexed by the opcode (`Exec::dispatch`, include/vm/vm.h). Its order is
# therefore the `Op` enum's, and nothing in the language holds that: a label
# address is not a constant expression, so the table cannot name its own
# indices, and the `static_assert` beside it can only count rows. A row in the
# wrong place compiles and runs a different instruction.
#
# `kNames` (the `dump` table) has the same shape and the same exposure, but it
# is checked every time anyone reads a `--vm-dump`, so it is the ordering that
# is actually held up by use. This gate says the two agree, which leaves one
# written-out ordering to keep against the enum instead of two.
set -euo pipefail
cd "$(dirname "$0")/../.."

vm=include/vm/vm.h

names=$(sed -n '/static constexpr const char\* kNames\[\] = {/,/};/p' "$vm" \
        | grep -o '"[A-Za-z0-9]\+"' | tr -d '"')
labels=$(sed -n '/static void\* const kLabels\[\] = {/,/^    };/p' "$vm" \
         | grep -o '&&L_[A-Za-z0-9]\+' | sed 's/^&&L_//')

n_names=$(printf '%s\n' "$names" | grep -c .)
n_labels=$(printf '%s\n' "$labels" | grep -c .)

if [[ "$n_names" -lt 100 || "$n_labels" -lt 100 ]]; then
  echo "vm-dispatch-table: parsed $n_names names and $n_labels labels --" \
       "one of the two tables moved and this gate stopped reading it" >&2
  exit 1
fi

if ! diff <(printf '%s\n' "$names") <(printf '%s\n' "$labels") > /tmp/vmdt.$$; then
  echo "vm-dispatch-table: kLabels and kNames disagree (kNames left," \
       "kLabels right). kLabels is indexed BY THE OPCODE, so a row out of" \
       "place runs the wrong instruction:" >&2
  cat /tmp/vmdt.$$ >&2
  rm -f /tmp/vmdt.$$
  exit 1
fi
rm -f /tmp/vmdt.$$

echo "vm-dispatch-table OK ($n_labels opcodes; kLabels order == kNames order)"
