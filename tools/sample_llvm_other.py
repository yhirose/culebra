#!/usr/bin/env python3
"""Sample the 'llvm::other' bucket: top symbols by size + top namespace prefixes."""
import re, subprocess, sys
from collections import Counter

binary = sys.argv[1] if len(sys.argv) > 1 else '/Users/yuji/Projects/culebra/build/culebra'

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
for i, (addr, t, name) in enumerate(raw):
    if t not in ('T', 't'):
        continue
    nxt = next((raw[j][0] for j in range(i+1, len(raw)) if raw[j][0] > addr), None)
    if nxt is None:
        continue
    size = nxt - addr
    if 0 < size <= 10*1024*1024:
        entries.append((name, size))

names = [e[0] for e in entries]
demangled = subprocess.run(['c++filt'], input='\n'.join(names), capture_output=True, text=True).stdout.rstrip('\n').split('\n')

known_targets = ['X86','AArch64','ARM','PowerPC','Mips','RISCV','WebAssembly','SystemZ','Sparc','AVR','AMDGPU','NVPTX','Hexagon','Lanai','BPF','LoongArch','VE','XCore','M68k','MSP430','Xtensa','CSKY','DirectX','SPIRV']
target_pat = re.compile(r'\bllvm::(' + '|'.join(known_targets) + r')\b')
component_re = [
    re.compile(r'\bllvm::orc::'),
    re.compile(r'\bllvm::MC[A-Z]|\bllvm::mc::'),
    re.compile(r'\bllvm::(MachineFunction|MachineInstr|SelectionDAG|LiveInterval|RegAlloc)'),
    re.compile(r'\bllvm::(IRBuilder|Module|Function|Instruction|BasicBlock|Constant|GlobalVariable|Type|Value)\b'),
    re.compile(r'\bllvm::(StringRef|SmallVector|DenseMap|StringMap|Triple|raw_|ErrorOr|Optional|cl::)'),
    re.compile(r'\bllvm::(LoopInfo|ScalarEvolution|AAResults|DominatorTree|MemorySSA|TargetLibraryInfo)'),
    re.compile(r'\bllvm::(PassBuilder|InstCombine|GVN|SimplifyCFG|LoopVectorize|SLPVectorizer)'),
    re.compile(r'\bllvm::(DWARF|DIE|DebugInfo|DIBuilder)'),
    re.compile(r'\bllvm::Bitcode'),
    re.compile(r'\bllvm::object::'),
]
def classified(name):
    if target_pat.search(name): return True
    for p in component_re:
        if p.search(name): return True
    return False

# Filter to llvm::other (unclassified llvm:: symbols)
other = [(n, s) for (_, s), n in zip(entries, demangled) if 'llvm::' in n and not classified(n)]
other.sort(key=lambda x: -x[1])

print(f"=== top 30 'llvm::other' symbols by size ===")
for n, s in other[:30]:
    print(f"  {s:>8,}  {n[:200]}")

# Look at second-level namespace under llvm::
ns2 = Counter()
for n, s in other:
    m = re.search(r'\bllvm::(\(anonymous namespace\)::)?([A-Za-z0-9_]+)', n)
    if m:
        ns2[m.group(2)] += s
    else:
        ns2['<other>'] += s

print()
print(f"=== top 30 second-level prefixes under llvm:: in 'other' ===")
for k, v in ns2.most_common(30):
    print(f"  {v/1024/1024:>6.2f} MB   llvm::{k}")
