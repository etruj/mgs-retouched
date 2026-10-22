# Sourced by the build scripts: locate the toolchain on this machine (override any variable
# by exporting it first). engine/tools/doctor.sh reports what was found.
_newest() { ls -d $1 2>/dev/null | sort -V | tail -1; }
for d in "${ANDROID_HOME:-}" "${ANDROID_SDK_ROOT:-}" "$HOME/Android/Sdk" "$HOME/Library/Android/sdk" /opt/android-sdk /usr/lib/android-sdk; do
  [ -n "$d" ] && [ -d "$d/platforms" ] && { ANDROID_SDK=${ANDROID_SDK:-$d}; break; }
done
ANDROID_SDK=${ANDROID_SDK:-}
ANDROID_NDK=${ANDROID_NDK:-${ANDROID_NDK_HOME:-$(_newest "$ANDROID_SDK/ndk/*")}}
ANDROID_BUILD_TOOLS=${ANDROID_BUILD_TOOLS:-$(_newest "$ANDROID_SDK/build-tools/*")}
ANDROID_JAR=${ANDROID_JAR:-$(_newest "$ANDROID_SDK/platforms/android-*")/android.jar}
_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
_deps=$_root/deps
# pinned downloads (engine/tools/fetch_deps.sh) win over system copies: same inputs, same APK
if [ -x "$_deps/wabt-1.0.37/build/wasm2c" ]; then
  WASM2C=${WASM2C:-$_deps/wabt-1.0.37/build/wasm2c}
  WABT_INCLUDE=${WABT_INCLUDE:-$_deps/wabt-1.0.37/wasm2c}; WABT_RUNTIME=${WABT_RUNTIME:-$_deps/wabt-1.0.37/wasm2c}
fi
WASM2C=${WASM2C:-$(command -v wasm2c 2>/dev/null || true)}
_w2c=$WASM2C
for d in "${WABT_INCLUDE:-}" "${_w2c:+$(dirname "$_w2c")/../include}" /usr/include /usr/local/include; do
  [ -n "$d" ] && [ -f "$d/wasm-rt.h" ] && { WABT_INCLUDE=$d; break; }
done
for d in "${WABT_RUNTIME:-}" "${_w2c:+$(dirname "$_w2c")/../share/wabt/wasm2c}" /usr/share/wabt/wasm2c /usr/local/share/wabt/wasm2c; do
  [ -n "$d" ] && [ -f "$d/wasm-rt-impl.c" ] && { WABT_RUNTIME=$d; break; }
done
WABT_INCLUDE=${WABT_INCLUDE:-}; WABT_RUNTIME=${WABT_RUNTIME:-}
_font() { for f in "$@"; do [ -f "$f" ] && { echo "$f"; return; }; done; }
[ -d "$_deps/fonts" ] && MGST_FONTS=${MGST_FONTS:-$_deps/fonts}
for d in "${MGST_FREETYPE:-}" "$_deps/freetype-2.13.3" "$_root/third_party/freetype-2.13.3"; do
  [ -n "$d" ] && [ -f "$d/include/ft2build.h" ] && { MGST_FREETYPE=$d; break; }
done
MGST_FREETYPE=${MGST_FREETYPE:-}
CLANG=${CLANG:-clang}
WASM_LD=${WASM_LD:-$(command -v wasm-ld 2>/dev/null || ls /usr/bin/wasm-ld-* 2>/dev/null | sort -V | tail -1 || true)}
FONT_REGULAR=${FONT_REGULAR:-$(_font "${MGST_FONTS:-/nonexistent}/LiberationSans-Regular.ttf" /usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf /usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf /usr/share/fonts/liberation/LiberationSans-Regular.ttf /usr/share/fonts/TTF/LiberationSans-Regular.ttf)}
FONT_BOLD=${FONT_BOLD:-$(_font "${MGST_FONTS:-/nonexistent}/LiberationSans-Bold.ttf" /usr/share/fonts/liberation-sans-fonts/LiberationSans-Bold.ttf /usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf /usr/share/fonts/liberation/LiberationSans-Bold.ttf /usr/share/fonts/TTF/LiberationSans-Bold.ttf)}
FONT_CJK=${FONT_CJK:-$(_font "${MGST_FONTS:-/nonexistent}/DroidSansFallbackFull.ttf" /usr/share/fonts/google-droid-sans-fonts/DroidSansFallbackFull.ttf /usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf /usr/share/fonts/droid/DroidSansFallbackFull.ttf /usr/share/fonts/TTF/DroidSansFallbackFull.ttf)}
export WASM2C CLANG WASM_LD MGST_FREETYPE MGST_FONTS ANDROID_SDK ANDROID_NDK ANDROID_BUILD_TOOLS ANDROID_JAR WABT_INCLUDE WABT_RUNTIME FONT_REGULAR FONT_BOLD FONT_CJK
