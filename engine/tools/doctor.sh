#!/usr/bin/env bash
# Check that this machine can build the APK; print how to fix anything missing.
ROOT=$(cd "$(dirname "$0")/../.." && pwd); cd "$ROOT"
. engine/tools/detect_env.sh
ok=1; T=$(mktemp -d)
pass() { printf '  %-28s ok   %s\n' "$1" "$2"; }
miss() { printf '  %-28s MISSING  %s\n' "$1" "$2"; ok=0; }
echo "Toolchain"
command -v python3 >/dev/null && pass python3 "$(python3 --version 2>&1)" || miss python3 "install python3"
for c in curl tar zip unzip sha256sum cmake make c++ git; do command -v $c >/dev/null && pass $c "" || miss $c "install $c"; done
if command -v "$CLANG" >/dev/null; then
  printf 'const unsigned char x[] = {\n#embed "%s"\n};\n' "$T/e" > "$T/e.c"; echo -n ab > "$T/e"
  if "$CLANG" --target=wasm32 -c "$T/e.c" -o "$T/e.o" 2>/dev/null && "$CLANG" --target=armv6-none-eabi -mabi=apcs-gnu -fsyntax-only -x c /dev/null 2>/dev/null
  then pass "clang (wasm32, armv6, #embed)" "$("$CLANG" --version | head -1)"
  else miss "clang >= 19 with wasm32" "Fedora: dnf install clang lld | Ubuntu: apt install clang-19 lld-19, then CLANG=clang-19"; fi
else miss clang "Fedora: dnf install clang lld | Ubuntu: apt install clang-19 lld-19"; fi
[ -n "$WASM_LD" ] && pass wasm-ld "$WASM_LD" || miss wasm-ld "Fedora: dnf install lld | Ubuntu: apt install lld-19 (or set WASM_LD)"
[ -n "$WASM2C" ] && [ -n "$WABT_RUNTIME" ] && [ -n "$WABT_INCLUDE" ] && pass "wasm2c + runtime" "$WASM2C" || miss "wasm2c + runtime" "run: ./build_apk.sh deps"
[ -n "$MGST_FREETYPE" ] && pass "FreeType source" "$MGST_FREETYPE" || miss "FreeType source" "run: ./build_apk.sh deps"
[ -n "$FONT_REGULAR" ] && [ -n "$FONT_BOLD" ] && [ -n "$FONT_CJK" ] && pass fonts "${MGST_FONTS:-system}" || miss fonts "run: ./build_apk.sh deps"
command -v keytool >/dev/null && pass "keytool (Java)" "" || miss "keytool (Java)" "Fedora: dnf install java-21-openjdk-headless | Ubuntu: apt install openjdk-17-jre-headless"
# RETOUCHED compiles one Java activity (javac, then d8 from build-tools)
command -v javac >/dev/null && pass "javac (JDK)" "" || miss "javac (JDK)" "Fedora: dnf install java-21-openjdk-devel | Ubuntu: apt install openjdk-17-jdk-headless"
echo "Android SDK"
[ -n "$ANDROID_SDK" ] && pass "SDK" "$ANDROID_SDK" || miss "SDK" "install Android Studio or the command-line tools; set ANDROID_HOME"
for t in aapt2 zipalign apksigner d8; do [ -x "$ANDROID_BUILD_TOOLS/$t" ] && pass "build-tools/$t" "$(basename "$ANDROID_BUILD_TOOLS")" || miss "build-tools/$t" 'sdkmanager "build-tools;34.0.0"'; done
[ -f "$ANDROID_JAR" ] && pass "platform android.jar" "$(basename "$(dirname "$ANDROID_JAR")")" || miss "platform android.jar" 'sdkmanager "platforms;android-34"'
[ -x "$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang" ] && pass NDK "$(basename "$ANDROID_NDK")" || miss NDK 'sdkmanager "ndk;27.3.13750724" (NDK r26 or newer)'
rm -rf "$T"
[ $ok = 1 ] && echo "Everything needed is present." || { echo "Fix the MISSING items above, then run this again."; exit 1; }
