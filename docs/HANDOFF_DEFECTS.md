# Defects found in the verified C while porting (for the decompilation project)

Both are in V0 / synthetic-only functions, outside what the differential tests executed.
The port fixes them at build time (`tools/xform.py`, `DEFECTS` and `FLOAT_BITS_RETURNERS`);
the files in `PORT_HANDOFF/src` are untouched. Each fix stops the build if its pattern turns
up anywhere new, so a regenerated handoff cannot silently reintroduce the class.

## 1. Unpadded struct size in pointer arithmetic: `SoundService::init` (0x21b44)

```c
this_01 = (Sound *)((int)&this_01[1].field0_0x0 + 3);
```
Ghidra models `struct Sound` as 0x39 bytes (no tail padding) and writes the 0x3c stride as
`&p[1] + 3`. Compiled as C, `sizeof(Sound)` is 0x3c (4-byte alignment, on armv6 and wasm32
alike), so the loop steps **0x3f**. Ports 2..n are constructed at the wrong offsets and read
as busy garbage: `No port can be used (null port)`, and no music ever plays.
**Fix:** `(Sound *)((int)this_01 + 0x3c)`. This is the only occurrence of the pattern in 1,395 files.

## 2. Float returned as `undefined4`, then converted instead of reinterpreted

`SoundManager::masterVolume`, `SoundManager::categoryVolume` and
`SoundResourceManager::attributeVolume` return the bits of a float as `undefined4` (on armv6
softfp a float return is just r0). Callers in `Sound::ready` (0x200d4, 2 sites) and
`SoundService::init` (0x21b44, 2 sites) write `(float)f()`, which **converts** the integer:
1.0f (0x3f800000) becomes 1.065e9, so the music volume became ~1.2e27.
**Fix:** `__BITCAST(float, f())`, the same treatment `COMPILE_REPORT.md` already applies to 42
implicit sites. The binary moves r0 to a VFP register unchanged.

## Also worth knowing

- `extern` declarations are emitted for identifiers that only appear in comments, for jump
  labels (`LAB_...`), and for names a parameter or local shadows (`type`, `target`). They are
  harmless when compiled against the original binary, but a port that resolves globals must
  respect C scoping.
- C++ vtables in the image hold original code addresses: 27 indirect call sites in the sound
  library call through them.
