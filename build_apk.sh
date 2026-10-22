#!/usr/bin/env bash
# METAL GEAR SOLID RETOUCHED: rebuild the game for Android from your own copy of MGS Touch.
#
#   ./build_apk.sh all --ipa GAME.ipa [--decomp DIR] [--yes] [--out FILE.apk]   everything: decompile, then build
#   ./build_apk.sh doctor                      check this machine (prints fixes)
#   ./build_apk.sh deps                        download the pinned helpers (wasm2c, FreeType, fonts)
#   ./build_apk.sh build --handoff DIR [--ipa GAME.ipa | --assets MGSTUS.app] [--out FILE.apk]
#
# "all" runs the decompiler (github.com/etruj/MetalGearSolidTouch-Decompilation-, cloned into
# deps/decomp at a tested commit unless --decomp points at your checkout) on your own IPA:
# reproduce.sh, then make_port_handoff.py, then the build. "build" starts from an existing
# make_port_handoff.py output folder. The APK contains the game: it is for your own devices.
set -euo pipefail
SELF=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")
ROOT=$(dirname "$SELF"); cd "$ROOT"
usage() { sed -n '4,12p' "$SELF" | sed 's/^# \{0,1\}//'; exit 2; }
[ -d engine ] && [ -d android-retouched ] || { echo "engine/ or android-retouched/ missing: this folder is incomplete"; exit 1; }
DECOMP_REPO=https://github.com/etruj/MetalGearSolidTouch-Decompilation-
DECOMP_COMMIT=9441736930437cd1c6866463dc5b23f2f6412453     # the decompiler version this port was tested with
abspath() { echo "$(cd "$(dirname "$1")" && pwd -P)/$(basename "$1")"; }
cmd=${1:-}; shift || true
case "$cmd" in
doctor) exec engine/tools/doctor.sh;;
deps) exec engine/tools/fetch_deps.sh;;
all)
    IPA=""; DECOMP=""; YES=""; OUT="$ROOT/out/mgs-retouched.apk"
    while [ $# -gt 0 ]; do
        case "$1" in
        --ipa) IPA=$2; shift 2;; --decomp) DECOMP=$2; shift 2;; --yes|-y) YES=--yes; shift;; --out) OUT=$2; shift 2;;
        *) usage;;
        esac
    done
    [ -n "$IPA" ] || usage
    [ -f "$IPA" ] || { echo "$IPA: no such file"; exit 1; }
    IPA=$(abspath "$IPA"); OUT=$(mkdir -p "$(dirname "$OUT")" && abspath "$OUT")
    # fail early, before the long decompile, if the machine cannot build the APK afterwards
    [ -d deps/wabt-1.0.37 ] && [ -d deps/freetype-2.13.3 ] && [ -f deps/fonts/DroidSansFallbackFull.ttf ] || engine/tools/fetch_deps.sh
    engine/tools/doctor.sh >/dev/null || { engine/tools/doctor.sh; exit 1; }
    if [ -z "$DECOMP" ]; then
        DECOMP=$ROOT/deps/decomp
        if [ ! -d "$DECOMP/.git" ]; then
            echo "== fetching the decompiler"; git clone -q "$DECOMP_REPO" "$DECOMP"
        fi
        git -C "$DECOMP" fetch -q origin "$DECOMP_COMMIT" 2>/dev/null || true
        git -C "$DECOMP" checkout -q "$DECOMP_COMMIT"
    fi
    DECOMP=$(cd "$DECOMP" && pwd -P)
    echo "== decompiling your IPA (the decompiler asks before installing anything; 15-30 minutes)"
    (cd "$DECOMP" && ./reproduce.sh "$IPA" --no-audio $YES)
    echo "== packaging the decompilation for the port"
    # make_port_handoff.py copies tools/port_handoff_README.md, which the public decompiler
    # repo (at DECOMP_COMMIT) does not contain; supply a stand-in so packaging completes
    if [ ! -f "$DECOMP/tools/port_handoff_README.md" ]; then
        printf '# MGS Touch decompilation: port handoff\n\nLOCAL ONLY: derived from your own copy of the game.\n' \
            > "$DECOMP/tools/port_handoff_README.md"
    fi
    (cd "$DECOMP" && python3 tools/make_port_handoff.py --out "$ROOT/build/handoff")
    exec "$SELF" build --handoff "$ROOT/build/handoff" --ipa "$IPA" --out "$OUT"
    ;;
build)
    HANDOFF=""; IPA=""; ASSETS=""; OUT="$ROOT/out/mgs-retouched.apk"
    while [ $# -gt 0 ]; do
        case "$1" in
        --handoff) HANDOFF=$2; shift 2;; --ipa) IPA=$2; shift 2;; --assets) ASSETS=$2; shift 2;; --out) OUT=$2; shift 2;;
        *) usage;;
        esac
    done
    [ -n "$HANDOFF" ] || usage
    HANDOFF=$(cd "$HANDOFF" && pwd -P); OUT=$(mkdir -p "$(dirname "$OUT")" && abspath "$OUT")
    [ -n "$IPA" ] && IPA=$(abspath "$IPA"); [ -n "$ASSETS" ] && ASSETS=$(cd "$ASSETS" && pwd -P)
    [ -d deps/wabt-1.0.37 ] && [ -d deps/freetype-2.13.3 ] && [ -f deps/fonts/DroidSansFallbackFull.ttf ] || engine/tools/fetch_deps.sh
    engine/tools/doctor.sh >/dev/null || { engine/tools/doctor.sh; exit 1; }
    if [ -n "$IPA" ]; then
        echo "== extracting the game's files from your IPA"
        mkdir -p build/assets; python3 engine/tools/extract_ipa.py "$IPA" build/assets; ASSETS=$ROOT/build/assets/MGSTUS.app
    fi
    ASSETS=${ASSETS:-$HANDOFF/assets/MGSTUS.app}
    python3 engine/tools/check_handoff.py "$HANDOFF" --assets "$ASSETS"
    echo "== 1/2 game module (transform the decompiled C, add the RETOUCHED runtime, build wasm, translate to C)"
    mkdir -p build; rm -f build/retouched/gen/mgst_w2c.c
    if ! MGST_HANDOFF=$HANDOFF MGST_EDITION=retouched engine/tools/build_wasm.sh > build/game_module.log 2>&1 \
       || [ ! -s build/retouched/gen/mgst_w2c.c ]; then
        echo "game module build failed; the end of build/game_module.log:"; tail -20 build/game_module.log; exit 1
    fi
    grep -E "^(mgst.wasm|wasm2c|patched|note:)" build/game_module.log || true
    echo "== 2/2 Android app (a few minutes: the translated module is one very large C file)"
    if ! MGST_HANDOFF=$HANDOFF MGST_ASSETS=$ASSETS MGST_APK=$OUT EDITION=retouched android-vanilla/build_android.sh \
         > build/android.log 2>&1; then
        echo "Android build failed; the end of build/android.log:"; tail -20 build/android.log; exit 1
    fi
    grep -E "^libmgst" build/android.log || true
    echo; echo "APK: $OUT ($(du -h "$OUT" | cut -f1))"
    echo "Install on a phone (USB debugging on):  adb install -r \"$OUT\""
    ;;
*) usage;;
esac
