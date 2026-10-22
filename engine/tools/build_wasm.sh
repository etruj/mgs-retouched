#!/usr/bin/env bash
# Build the game module: verified C (transformed) + runtime -> mgst.wasm -> wasm2c C.
#   MGST_EDITION=original (default): build/mgst.wasm, build/gen/mgst_w2c.c
#   MGST_EDITION=retouched: same game objects + runtime with the Re-Touched hooks and
#     android-retouched/rt -> build/retouched/mgst.wasm, build/retouched/gen/mgst_w2c.c
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd); cd "$ROOT"
export MGST_HANDOFF=${MGST_HANDOFF:-$ROOT/PORT_HANDOFF}; HANDOFF=$MGST_HANDOFF
. engine/tools/detect_env.sh
B=build; G=$B/gen; mkdir -p $G/include $B/wobj $B/robj
. engine/tools/wasm_flags.sh
python3 engine/tools/mk_image.py $G >/dev/null
python3 engine/tools/mk_header.py $HANDOFF/include/mgst_types.fixed.h $G/include/mgst_types.fixed.h
cp $HANDOFF/include/ghidra_ops.h $G/include/
engine/tools/layout_check.sh $G/include
HREAL=$(cd "$HANDOFF" && pwd -P)
if [ ! -f $G/.xform.stamp ] || [ engine/tools/xform.py -nt $G/.xform.stamp ] || [ $HANDOFF/index.tsv -nt $G/.xform.stamp ] \
   || [ "$(cat $G/.xform.stamp)" != "$HREAL" ]; then
  rm -rf $G/game $B/wobj; mkdir -p $B/wobj; python3 engine/tools/xform.py $G; echo "$HREAL" > $G/.xform.stamp
fi
python3 engine/tools/mk_imports.py $G/imports.txt >/dev/null
python3 engine/tools/mk_autostubs.py $G/imports.txt $G/autostubs.c
INC="-I $G/include -I $G -I engine/rt"
# game files: only rebuild what changed
ls $G/game/*.c | while read f; do o=$B/wobj/$(basename "$f" .c).o; [ "$o" -nt "$f" ] && [ "$o" -nt engine/rt/port_game.h ] || echo "$f"; done \
  | xargs -r -P "$(nproc)" -I{} sh -c '"$CLANG" '"$GAME_CFLAGS"' '"$INC"' -c {} -o '"$B"'/wobj/$(basename {} .c).o'
RT_CFLAGS="--target=wasm32 -O2 -std=gnu23 -ffreestanding -fno-builtin -nostdlib -mnontrapping-fptoint -mbulk-memory -ffp-contract=off -fno-strict-aliasing -Wall -Wno-unused-function -Wno-pointer-sign -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-unused-variable"
EDITION=${MGST_EDITION:-original}
if [ "$EDITION" = retouched ]; then
  RO=$B/robj-retouched; OUTW=$B/retouched/mgst.wasm; OUTG=$B/retouched/gen; RT_CFLAGS="$RT_CFLAGS -DMGST_RETOUCHED"
  RT_SRC="engine/rt/*.c android-retouched/rt/*.c"; GAME_SIDE="android-retouched/rt/game/*.c"
else
  RO=$B/robj; OUTW=$B/mgst.wasm; OUTG=$G; RT_SRC="engine/rt/*.c"; GAME_SIDE=""
fi
rm -rf $RO; mkdir -p $RO $OUTG
for f in $RT_SRC $G/image_meta.c $G/invoke_orig.c $G/autostubs.c $GAME_SIDE; do
  o=$RO/$(basename "$f" .c).o
  case $f in */invoke_orig.c|*/autostubs.c|*/rt/game/*) FL="$GAME_CFLAGS -I engine/rt";; *) FL="$RT_CFLAGS";; esac
  $CLANG $FL $INC --embed-dir=$G -c "$f" -o "$o"
done
"$WASM_LD" --no-entry --global-base=1048576 -z stack-size=2097152 --initial-memory=134217728 --max-memory=2147483648 \
  --export=__stack_pointer --export=__heap_base --export-table --growable-table --fatal-warnings \
  -o $OUTW $B/wobj/*.o $RO/*.o
echo "mgst.wasm ($EDITION): $(stat -c %s $OUTW) bytes"
"$WASM2C" --module-name=mgst -o $OUTG/mgst_w2c.c $OUTW
# the armv6 had no divide instruction: the runtime helpers returned 0 for x/0 and INT_MIN for
# INT_MIN/-1 (measured, runtime/vfpshim). wasm traps instead; give the original semantics back.
python3 engine/tools/patch_w2c.py $OUTG/mgst_w2c.c
echo "wasm2c: $(wc -l < $OUTG/mgst_w2c.c) lines"
