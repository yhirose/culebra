#!/usr/bin/env python3
"""Categorize culebra binary code-size contribution by LLVM target/component.

Approximates per-symbol size from sorted nm -n output (Mach-O nm does not
emit sizes). Each T/t symbol's size is `next_addr - this_addr`.
"""
import re
import subprocess
import sys
from pathlib import Path

binary = sys.argv[1] if len(sys.argv) > 1 else 'build/culebra'

proc = subprocess.run(['nm', '-n', binary], capture_output=True, text=True)
raw = []
for line in proc.stdout.splitlines():
    parts = line.split(None, 2)
    if len(parts) != 3:
        continue
    try:
        addr = int(parts[0], 16)
    except ValueError:
        continue
    raw.append((addr, parts[1], parts[2]))
raw.sort()

entries = []
total = 0
for i, (addr, t, name) in enumerate(raw):
    if t not in ('T', 't'):
        continue
    nxt = None
    for j in range(i + 1, len(raw)):
        if raw[j][0] > addr:
            nxt = raw[j][0]
            break
    if nxt is None:
        continue
    size = nxt - addr
    if size <= 0 or size > 10 * 1024 * 1024:
        continue
    entries.append((name, size))
    total += size

names = [e[0] for e in entries]
demangled = subprocess.run(
    ['c++filt'], input='\n'.join(names),
    capture_output=True, text=True,
).stdout.rstrip('\n').split('\n')

# LLVM backend targets (folder names under llvm/lib/Target/).
target_aliases = {
    'X86':         [r'llvm::X86', r'\(anonymous namespace\)::X86'],
    'AArch64':     [r'llvm::AArch64', r'\(anonymous namespace\)::AArch64'],
    'ARM':         [r'llvm::ARM(?!V[0-9]*Reg)', r'\(anonymous namespace\)::ARM'],
    'PowerPC':     [r'llvm::P(?:PC|owerPC)', r'\(anonymous namespace\)::PPC'],
    'Mips':        [r'llvm::Mips', r'\(anonymous namespace\)::Mips'],
    'RISCV':       [r'llvm::RISCV', r'\(anonymous namespace\)::RISCV'],
    'WebAssembly': [r'llvm::(?:WebAssembly|WASM)', r'\(anonymous namespace\)::(?:WebAssembly|WASM)'],
    'SystemZ':     [r'llvm::SystemZ', r'\(anonymous namespace\)::SystemZ'],
    'Sparc':       [r'llvm::Sparc', r'\(anonymous namespace\)::Sparc'],
    'AVR':         [r'llvm::AVR', r'\(anonymous namespace\)::AVR'],
    # AMDGPU has GCN/SI/R600 sub-prefixes too.
    'AMDGPU':      [r'llvm::(?:AMDGPU|GCN|SI[A-Z]|R600)', r'\(anonymous namespace\)::(?:AMDGPU|GCN|SI[A-Z]|R600)'],
    'NVPTX':       [r'llvm::NVPTX', r'\(anonymous namespace\)::NVPTX'],
    'Hexagon':     [r'llvm::Hexagon', r'\(anonymous namespace\)::Hexagon'],
    'Lanai':       [r'llvm::Lanai', r'\(anonymous namespace\)::Lanai'],
    'BPF':         [r'llvm::BPF', r'\(anonymous namespace\)::BPF'],
    'LoongArch':   [r'llvm::LoongArch', r'\(anonymous namespace\)::LoongArch'],
    'VE':          [r'llvm::VE[A-Z]', r'\(anonymous namespace\)::VE[A-Z]'],
    'XCore':       [r'llvm::XCore', r'\(anonymous namespace\)::XCore'],
    'M68k':        [r'llvm::M68k', r'\(anonymous namespace\)::M68k'],
    'MSP430':      [r'llvm::MSP430', r'\(anonymous namespace\)::MSP430'],
    'Xtensa':      [r'llvm::Xtensa', r'\(anonymous namespace\)::Xtensa'],
    'CSKY':        [r'llvm::CSKY', r'\(anonymous namespace\)::CSKY'],
    'DirectX':     [r'llvm::(?:DirectX|DXIL)', r'\(anonymous namespace\)::(?:DirectX|DXIL)'],
    'SPIRV':       [r'llvm::SPIRV', r'\(anonymous namespace\)::SPIRV'],
}
targets = list(target_aliases.keys())
target_re = {t: re.compile('|'.join(target_aliases[t])) for t in targets}

# Non-target LLVM components (rough buckets by namespace prefix).
component_re = [
    ('llvm::orc',        re.compile(r'\bllvm::orc::')),
    ('llvm::mc',         re.compile(r'\bllvm::MC[A-Z]|\bllvm::mc::')),
    ('llvm::codegen',    re.compile(r'\bllvm::(MachineFunction|MachineInstr|SelectionDAG|LiveInterval|RegAlloc)')),
    ('llvm::ir',         re.compile(r'\bllvm::(IRBuilder|Module|Function|Instruction|BasicBlock|Constant|GlobalVariable|Type|Value)\b')),
    ('llvm::support',    re.compile(r'\bllvm::(StringRef|SmallVector|DenseMap|StringMap|Triple|raw_|ErrorOr|Optional|cl::)')),
    ('llvm::analysis',   re.compile(r'\bllvm::(LoopInfo|ScalarEvolution|AAResults|DominatorTree|MemorySSA|TargetLibraryInfo)')),
    ('llvm::transforms', re.compile(r'\bllvm::(PassBuilder|InstCombine|GVN|SimplifyCFG|LoopVectorize|SLPVectorizer)')),
    ('llvm::debuginfo',  re.compile(r'\bllvm::(DWARF|DIE|DebugInfo|DIBuilder)')),
    ('llvm::bitcode',    re.compile(r'\bllvm::Bitcode')),
    ('llvm::object',     re.compile(r'\bllvm::object::')),
]

target_size = {t: [0, 0] for t in targets}
comp_size = {k: [0, 0] for k, _ in component_re}
llvm_other = [0, 0]
non_llvm = [0, 0]

for (mangled, size), name in zip(entries, demangled):
    matched = False
    for t, p in target_re.items():
        if p.search(name):
            target_size[t][0] += 1
            target_size[t][1] += size
            matched = True
            break
    if matched:
        continue
    for k, p in component_re:
        if p.search(name):
            comp_size[k][0] += 1
            comp_size[k][1] += size
            matched = True
            break
    if matched:
        continue
    if 'llvm::' in name:
        llvm_other[0] += 1
        llvm_other[1] += size
    else:
        non_llvm[0] += 1
        non_llvm[1] += size

print(f"binary: {binary}")
print(f"approx code (text): {total/1024/1024:.2f} MB across {len(entries):,} symbols")
print()
print("=== LLVM TARGETS (cross-compile backends) ===")
print(f"{'target':<14} {'symbols':>9}   {'MB':>7}   {'%text':>6}")
target_total = 0
for t, (c, b) in sorted(target_size.items(), key=lambda kv: -kv[1][1]):
    if c == 0:
        continue
    target_total += b
    print(f"  {t:<12} {c:>9,}   {b/1024/1024:>7.2f}   {b/total*100:>5.2f}%")
print(f"  {'(targets sum)':<12} {'':>9}   {target_total/1024/1024:>7.2f}   {target_total/total*100:>5.2f}%")
print()
print("=== OTHER LLVM COMPONENTS ===")
print(f"{'component':<14} {'symbols':>9}   {'MB':>7}   {'%text':>6}")
comp_total = 0
for k, (c, b) in sorted(comp_size.items(), key=lambda kv: -kv[1][1]):
    comp_total += b
    print(f"  {k:<12} {c:>9,}   {b/1024/1024:>7.2f}   {b/total*100:>5.2f}%")
print(f"  llvm::other  {llvm_other[0]:>9,}   {llvm_other[1]/1024/1024:>7.2f}   {llvm_other[1]/total*100:>5.2f}%")
print()
print("=== NON-LLVM ===")
print(f"  {non_llvm[0]:>9,} symbols   {non_llvm[1]/1024/1024:>7.2f} MB   {non_llvm[1]/total*100:>5.2f}%")
