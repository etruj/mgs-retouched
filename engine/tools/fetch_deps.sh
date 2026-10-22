#!/usr/bin/env bash
# Download the pinned third-party pieces into deps/ (checksums verified), so every machine
# builds the same APK: wabt 1.0.37 (wasm2c, built from source), FreeType 2.13.3, Liberation Sans 2.1.5
# (SIL OFL), Droid Sans Fallback (Apache 2.0).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd); D=$ROOT/deps; mkdir -p "$D/fonts" "$D/dl"
get() {   # url sha256 file
    local f="$D/dl/$3"
    if [ ! -f "$f" ] || ! echo "$2  $f" | sha256sum -c --status; then
        echo "downloading $3"; curl -fsSL --retry 3 -o "$f" "$1"
    fi
    echo "$2  $f" | sha256sum -c --status || { echo "checksum mismatch for $3"; exit 1; }
}
get https://github.com/WebAssembly/wabt/releases/download/1.0.37/wabt-1.0.37.tar.xz \
    aca7fc4ba9974ad456372ab35f9cc1bac0b9fab3321b6df399d14dda9ca8a1a9 wabt-1.0.37-src.tar.xz
get https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.xz \
    0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289 freetype-2.13.3.tar.xz
get https://github.com/liberationfonts/liberation-fonts/files/7261482/liberation-fonts-ttf-2.1.5.tar.gz \
    7191c669bf38899f73a2094ed00f7b800553364f90e2637010a69c0e268f25d0 liberation-fonts-2.1.5.tar.gz
get https://raw.githubusercontent.com/aosp-mirror/platform_frameworks_base/android-5.1.1_r1/data/fonts/DroidSansFallbackFull.ttf \
    acb6440a713d880a13a21b468ba7cd43f5a2b2934972e51be791c880730777b8 DroidSansFallbackFull.ttf
# wasm2c from source (the prebuilt release needs OpenSSL 1.1, gone from current distributions)
if [ ! -x "$D/wabt-1.0.37/build/wasm2c" ]; then
    [ -d "$D/wabt-1.0.37" ] || tar xJf "$D/dl/wabt-1.0.37-src.tar.xz" -C "$D"
    echo "building wasm2c (one time, about a minute)"
    cmake -S "$D/wabt-1.0.37" -B "$D/wabt-1.0.37/build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF \
          -DBUILD_LIBWASM=OFF -DUSE_INTERNAL_SHA256=ON -DWITH_WASI=OFF >/dev/null
    cmake --build "$D/wabt-1.0.37/build" --target wasm2c -j "$(nproc)" >/dev/null
fi
[ -d "$D/freetype-2.13.3" ] || tar xJf "$D/dl/freetype-2.13.3.tar.xz" -C "$D"
tar xzf "$D/dl/liberation-fonts-2.1.5.tar.gz" -C "$D/dl" liberation-fonts-ttf-2.1.5/LiberationSans-Regular.ttf liberation-fonts-ttf-2.1.5/LiberationSans-Bold.ttf
cp "$D/dl/liberation-fonts-ttf-2.1.5/LiberationSans-"{Regular,Bold}.ttf "$D/fonts/"
cp "$D/dl/DroidSansFallbackFull.ttf" "$D/fonts/"
echo "deps ready in $D"
