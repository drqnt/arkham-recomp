# arkham-recomp

Personal project: run my own Steam copy of *Batman: Arkham Asylum GOTY* (the 32-bit Feral Mac build) natively on Apple
Silicon, and later on iPad and iPhone, by statically recompiling its i386 code to ARM64.

**This repository holds tools and a runtime only.** It has no game code and no game assets. `gen/`, `build/` and `logs/`
are derived from the installed game, stay on this machine, and must never be published.

## Build and run

```
python3 -I tools/lift.py     # i386 Mach-O -> gen/*.c (about 10 s)
python3 -I tools/build.py    # gen + runtime -> build/arkham (about a minute from clean)
tools/run.sh                 # Steam must be running
python3 -I tests/difftest.py 400   # instruction semantics against real x86 under Rosetta
```

## Layout

- `tools/lift.py`, `emit.py`, `x86.py`, `macho.py`: the recompiler. `gen_gl.py`: OpenGL shims from SDK headers.
- `tools/showcode.py`, `xref.py`, `vtable.py`, `objcdump.py`, `hook_callees.py`, `config/hooks.json`: reverse-engineering aids.
- `tools/try.sh`: rebuild and run briefly with a compact summary.
- `runtime/rt.c`, `rt.h`, `rt_ops.h`: guest memory, dispatch, thread gate, SSE and x87 semantics.
- `runtime/shims_*.c`: libc, threads and GCD, GNU libstdc++, CoreFoundation, Objective-C bridge, OpenGL and CGL, Steam.

## Limits

No leaked engine source. No bypassing Steam or copy protection: ownership is answered by the running Steam client through
Valve's own arm64 `libsteam_api`, found in the local Steam library. Networking stays off: the updater gets a local 404.

## Status

Working: all 659 static initializers, `main`, the real `[NSApp run]`, Feral's HTML launcher through Play, the Steam check,
Unreal Engine 3 config generation and package scanning, and the start of window creation.

Not working yet: no frame has been rendered. Next are CoreText fonts, the first frame through OpenGL, Bink video, OpenAL
audio, gamepads and Steam callbacks. `ARKHAM_STUB_ALL=1` still makes imports without a shim return 0.
