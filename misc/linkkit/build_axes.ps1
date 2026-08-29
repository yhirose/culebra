# Build and run every AOT feature axis with the kit that is currently
# installed, checking each program's output against misc/linkkit/expected.tsv.
#
# Usage: pwsh misc/linkkit/build_axes.ps1 <culebra.exe>
#
# Each axis is built as <name>.exe in the current directory and left there: the
# caller in ci.yml runs hello.exe again after `toolchain uninstall`, to prove a
# produced binary needs nothing from the kit.
#
# Every axis, not just hello: each appends its own import libraries, and a kit
# packed from a hello link would be missing them — the failure would otherwise
# surface only for whoever first compiles a Canvas program.
#
# Called by ci.yml (from a locally packed kit) and toolchain-smoke.yml (from the
# published one). Kept in a file rather than inline in both workflows because a
# copy in the network job is the copy nobody runs on a push, so it is the one
# that silently rots — and because assembling PowerShell inside YAML is how the
# spike lost five axes to a quoting bug that looked like a link failure.
param([Parameter(Mandatory=$true)][string]$Exe)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSCommandPath

foreach ($line in Get-Content (Join-Path $root 'expected.tsv')) {
  $name, $want = $line -split "`t"
  & $Exe build (Join-Path $root "$name.cul") -o "$name.exe"
  if ($LASTEXITCODE -ne 0) { throw "$name did not link" }
  $got = (& ".\$name.exe" 2>&1 | Out-String).Trim()
  if ($got -ne $want) { throw "$name ran as '$got', wanted '$want'" }
  "  ok: $name -> $got"
}
