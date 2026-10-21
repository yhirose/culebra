#!/usr/bin/env bash
# The number of jobs of <bytes> each that this machine's RAM holds, at least
# one and at most one per core: the parallelism for work whose fan-out is
# bounded by memory before cores.
#
# A compile of this tree is one: each translation unit includes most of the
# runtime and takes 0.6-6 GB, while the build's wall time is a serial chain
# (preamble tool -> baked modules -> runtime archive -> driver -> link). One
# job per core only adds memory: on a 20-thread 16 GB machine it ran 17 units
# at once and out of memory, and finished no sooner than 8 jobs.
#
# The physical size, not what is free right now, so the answer is the same
# from run to run. Where the size cannot be read, one job per core.
#
# usage: jobs_within.sh <bytes-per-job>
set -euo pipefail

per_job=${1:?usage: jobs_within.sh <bytes-per-job>}
cores=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null \
  || echo 8)
mem=$(sysctl -n hw.memsize 2>/dev/null || true)
if [[ -z "$mem" ]]; then
  pages=$(getconf _PHYS_PAGES 2>/dev/null || true)
  page=$(getconf PAGE_SIZE 2>/dev/null || echo 4096)
  [[ "$pages" =~ ^[0-9]+$ ]] && mem=$(( pages * page ))
fi
if [[ -z "$mem" && -r /proc/meminfo ]]; then
  mem=$(awk '/^MemTotal:/ { print $2 * 1024 }' /proc/meminfo)
fi
if ! [[ "$mem" =~ ^[0-9]+$ ]] || (( mem == 0 )); then
  echo "$cores"
  exit 0
fi
n=$(( mem / per_job ))
(( n >= 1 )) || n=1
(( n < cores )) || n=$cores
echo "$n"
