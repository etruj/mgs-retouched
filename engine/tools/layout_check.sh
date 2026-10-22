#!/usr/bin/env bash
# Every struct in mgst_types.fixed.h must have the same layout on wasm32 (generated
# header) as on the original armv6 apcs-gnu target.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd); T=$(mktemp -d)
HANDOFF=${MGST_HANDOFF:-$ROOT/PORT_HANDOFF}
python3 - "$HANDOFF/include/mgst_types.fixed.h" "$T/lay.c" <<'PY'
import re, sys
names = sorted(set(re.findall(r'^struct (\w+) \{', open(sys.argv[1]).read(), re.M)))
open(sys.argv[2], 'w').write('#include "mgst_types.fixed.h"\n' + ''.join(f'int s{i}=sizeof(struct {n});\n' for i, n in enumerate(names)))
PY
${CLANG:-clang} --target=armv6-none-eabi -mabi=apcs-gnu -mfloat-abi=softfp -fsigned-char -fno-short-enums -w -I "$HANDOFF/include" \
  -Xclang -fdump-record-layouts-simple -fsyntax-only "$T/lay.c" > "$T/arm.txt"
${CLANG:-clang} --target=wasm32 -fsigned-char -fno-short-enums -w -I "$1" \
  -Xclang -fdump-record-layouts-simple -fsyntax-only "$T/lay.c" > "$T/wasm.txt"
# alignment may legitimately be reported differently; sizes and offsets may not
n=$(diff <(grep -vE 'Alignment' "$T/arm.txt") <(grep -vE 'Alignment' "$T/wasm.txt") | grep -c '^<' || true)
echo "struct layout differences (size/offsets) armv6 vs wasm32: $n"
rm -rf "$T"; [ "$n" = 0 ]
