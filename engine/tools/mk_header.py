#!/usr/bin/env python3
"""Generate the wasm32 copy of mgst_types.fixed.h. armv6 apcs-gnu aligns 64-bit
scalars to 4 bytes; wasm32 aligns them to 8. Every struct field of a 64-bit type is
marked packed+aligned(4) so the layouts match the original byte for byte. The
check is `engine/tools/layout_check.sh` (0 differences)."""
import re, sys
src, dst = sys.argv[1], sys.argv[2]
W64 = r'(?:double|longlong|ulonglong|qword|undefined8|__int64_t|__darwin_off_t|long long|unsigned long long|fpos_t)'
out, depth = [], 0
for line in open(src):
    if depth > 0:
        line = re.sub(rf'^(\s+{W64}\s+\w+(?:\[\d+\])*)\s*;', r'\1 __attribute__((packed, aligned(4)));', line)
    depth += line.count('{') - line.count('}')
    out.append(line)
open(dst, 'w').write(''.join(out))
