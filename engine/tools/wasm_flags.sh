# shared compile flags for everything that goes into the wasm32 module
GAME_CFLAGS="--target=wasm32 -O1 -std=gnu17 -ffreestanding -fno-builtin -nostdlib -fsigned-char -fno-short-enums \
 -mnontrapping-fptoint -mbulk-memory -ffp-contract=off -fno-strict-aliasing -w \
 -Wno-error=int-conversion -Wno-error=incompatible-pointer-types -Wno-error=implicit-function-declaration \
 -Wno-error=incompatible-function-pointer-types -Wno-error=return-type -Wno-error=int-to-pointer-cast"
