#!/usr/bin/env bash
# Android build of the port: arm64-v8a (+ x86_64 for emulators), offline, no Gradle.
# Needs the game module from engine/tools/build_wasm.sh. The APK carries the game bundle from
# $MGST_ASSETS (default: the handoff's assets/MGSTUS.app): build it for your own devices only.
#   EDITION=original (default): the exact port, com.mgsport.touch -> build/mgst-touch.apk
#   EDITION=retouched: METAL GEAR SOLID RE-TOUCHED, com.mgsport.retouched -> build/mgst-retouched.apk
#     (use android-retouched/build_retouched.sh, which builds its module first)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$ROOT"
HANDOFF=${MGST_HANDOFF:-$ROOT/PORT_HANDOFF}
ASSETS=${MGST_ASSETS:-$HANDOFF/assets/MGSTUS.app}
EDITION=${EDITION:-original}
if [ "$EDITION" = retouched ]; then
  APK=${MGST_APK:-build/mgst-retouched.apk}; B=build/android-retouched; GEN=build/retouched/gen
  MANIFEST=android-retouched/app/AndroidManifest.xml
  EXTRA_CF="-DMGST_RETOUCHED -I android-retouched/host"; EXTRA_SRC="android-retouched/host/rtx_host.c android-retouched/host/rtx_ui.c"
else
  APK=${MGST_APK:-build/mgst-touch.apk}; B=build/android; GEN=build/gen
  MANIFEST=android-vanilla/app/AndroidManifest.xml; EXTRA_CF=""; EXTRA_SRC=""
fi
. engine/tools/detect_env.sh     # SDK, NDK, build-tools, platform, wabt, fonts
BT=$ANDROID_BUILD_TOOLS; JAR=$ANDROID_JAR; NDK=$ANDROID_NDK; TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
API=28; ABIS=${ABIS:-"arm64-v8a x86_64"}   # x86_64: for the local emulator
mkdir -p $B/wabt
FT=$MGST_FREETYPE
[ -n "$FT" ] || { engine/tools/fetch_deps.sh; . engine/tools/detect_env.sh; FT=$MGST_FREETYPE; }
cp "$WABT_INCLUDE/wasm-rt.h" "$WABT_INCLUDE/wasm-rt-exceptions.h" $B/wabt/
W=$WABT_RUNTIME
GLUE=$NDK/sources/android/native_app_glue
COMMON="-O2 -fPIC -I $B/wabt -I $W -I $GEN -I engine/host -DWASM_RT_MEMCHECK_GUARD_PAGES=1"
FTF="-O2 -fPIC -DFT2_BUILD_LIBRARY -DFT_CONFIG_MODULES_H=\"ftmodule.h\" -I engine/host -I $FT/include -w"
for ABI in $ABIS; do
  case $ABI in arm64-v8a) TRI=aarch64-linux-android;; x86_64) TRI=x86_64-linux-android;; esac
  CC="$TC/$TRI$API-clang"; O=$B/obj-$ABI; mkdir -p $O $B/apk/lib/$ABI
  if [ ! -f $O/mgst_w2c.o ] || [ $GEN/mgst_w2c.c -nt $O/mgst_w2c.o ]; then
    echo "compiling generated module for $ABI ($(wc -l < $GEN/mgst_w2c.c) lines)..."
    $CC $COMMON -w -fno-strict-aliasing -ffp-contract=off -c $GEN/mgst_w2c.c -o $O/mgst_w2c.o
  fi
  for f in $W/wasm-rt-impl.c $W/wasm-rt-exceptions-impl.c $W/wasm-rt-mem-impl.c; do $CC $COMMON -w -c $f -o $O/$(basename $f .c).o; done
  for f in base/ftsystem.c base/ftinit.c base/ftdebug.c base/ftbase.c base/ftbitmap.c base/ftglyph.c base/ftmm.c \
           truetype/truetype.c sfnt/sfnt.c smooth/smooth.c autofit/autofit.c psnames/psnames.c gzip/ftgzip.c; do
    o=$O/ft_$(basename $f .c).o; [ -f $o ] || $CC $FTF -c $FT/src/$f -o $o
  done
  $CC $COMMON -w -I $GLUE -c $GLUE/android_native_app_glue.c -o $O/glue.o
  for f in engine/host/vfs.c engine/host/touch.c engine/host/core.c engine/host/gfx.c engine/host/image.c engine/host/mixer.c engine/host/text_ft.c android-vanilla/host/main.c $EXTRA_SRC; do
    $CC $COMMON $EXTRA_CF -DHOST_GLES -I $GLUE -I $FT/include -Wall -Wno-unused-function -Wno-unused-variable -c $f -o $O/h_$(basename $f .c).o
  done
  $CC -shared -o $B/apk/lib/$ABI/libmgst.so $O/*.o -u ANativeActivity_onCreate -Wl,--no-undefined -Wl,-z,max-page-size=16384 \
    -landroid -llog -lEGL -lGLESv1_CM -laaudio -lz -lm
  echo "libmgst.so ($ABI): $(stat -c %s $B/apk/lib/$ABI/libmgst.so) bytes"
done
# assets: the original bundle + fonts (Liberation Sans: SIL OFL; Droid Sans Fallback: Apache 2.0)
A=$B/assets; mkdir -p $A/fonts
rm -f $A/MGSTUS.app; ln -s "$(cd "$ASSETS" && pwd -P)" $A/MGSTUS.app
cp "$FONT_REGULAR" $A/fonts/LiberationSans-Regular.ttf; cp "$FONT_BOLD" $A/fonts/LiberationSans-Bold.ttf
cp "$FONT_CJK" $A/fonts/DroidSansFallbackFull.ttf
RES=""
if [ "$EDITION" = retouched ]; then
  # launcher icon (made from the bundle's own icon) and the full-screen activity (Java -> dex)
  rm -rf $B/res $B/java $B/dex; mkdir -p $B/res/mipmap-xxhdpi $B/java $B/dex
  mkdir -p $A/overlay; cp android-retouched/overlay/* $A/overlay/; cp android-retouched/fonts/BebasNeue-Regular.ttf $A/fonts/
  python3 android-retouched/app/make_icon.py "$ASSETS" $B/res/mipmap-xxhdpi/ic_launcher.png
  $BT/aapt2 compile --dir $B/res -o $B/res.zip; RES="-R $B/res.zip"
  javac -nowarn -Xlint:-options --release 8 -cp "$JAR" -d $B/java $(find android-retouched/app/java -name '*.java')
  $BT/d8 --min-api $API --lib "$JAR" --output $B/dex $(find $B/java -name '*.class')
fi
$BT/aapt2 link --manifest $MANIFEST -I $JAR --min-sdk-version $API --target-sdk-version 34 \
  -A $A $RES --auto-add-overlay --no-compress -o $B/unsigned.apk
(cd $B/apk && zip -q -0 -r ../unsigned.apk lib)
[ "$EDITION" = retouched ] && (cd $B/dex && zip -q ../unsigned.apk classes.dex)
$BT/zipalign -f -p 16 $B/unsigned.apk $B/aligned.apk
KS=build/debug.keystore
[ -f $KS ] || keytool -genkeypair -keystore $KS -storepass android -keypass android -alias debug \
  -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=mgsport local build" >/dev/null 2>&1
mkdir -p "$(dirname "$APK")"
$BT/apksigner sign --ks $KS --ks-pass pass:android --out "$APK" $B/aligned.apk 2>/dev/null
$BT/apksigner verify "$APK" 2>/dev/null && ls -l "$APK"
