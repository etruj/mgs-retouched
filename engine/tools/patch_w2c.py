#!/usr/bin/env python3
"""Give wasm2c's integer division the semantics of the original device.
armv6 has no divide instruction; the runtime helpers the game was linked against
(measured, PORT_HANDOFF/runtime/vfpshim) return x/0 = 0, x%0 = 0, INT_MIN/-1 = INT_MIN
and INT_MIN%-1 = 0. WebAssembly traps on the first three instead."""
import sys, re
p = sys.argv[1]; s = open(p).read()
repl = {
 'DIV_S': '#define DIV_S(ut, min, x, y) \\\n  ((UNLIKELY((y) == 0)) ? (ut)0 : (UNLIKELY((x) == min && (y) == -1)) ? (ut)(min) : (ut)((x) / (y)))\n',
 'REM_S': '#define REM_S(ut, min, x, y) \\\n  ((UNLIKELY((y) == 0)) ? (ut)0 : (UNLIKELY((x) == min && (y) == -1)) ? (ut)0 : (ut)((x) % (y)))\n',
 'DIVREM_U': '#define DIVREM_U(op, x, y) \\\n  ((UNLIKELY((y) == 0)) ? 0 : ((x)op(y)))\n',
}
n = 0
lines = s.split('\n')
out = []; i = 0
while i < len(lines):
    m = re.match(r'#define (\w+)\(', lines[i])
    if m and m.group(1) in repl:
        while lines[i].rstrip().endswith('\\'): i += 1      # skip the whole multi-line macro
        out.append(repl[m.group(1)].rstrip('\n')); n += 1; i += 1; continue
    out.append(lines[i]); i += 1
s = '\n'.join(out)
if n != 3: sys.exit(f'patch_w2c: expected 3 macros, patched {n}')
open(p, 'w').write(s)
print('patched integer division: armv6 helper semantics')
