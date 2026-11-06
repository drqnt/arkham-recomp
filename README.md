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

Working, seen in test runs and in play by hand with the real mouse: the launcher and the real Steam check, logos, title screen and menus, New Game, the opening
Bink cutscenes, and real-time play in the Intensive Treatment lobby at about 2000 draw calls a frame. W walks, the camera
turns with mouse motion, Esc opens the pause menu, and memory stays near 1.3 GB over a 5-minute run. FMOD's sound and Bink's
video sound both start, and saves go through the real Steam Cloud. The Bink library is translated as a second image at `0xE1000000`.

Speed, measured in the first level on an M5 Pro at 2560x1600: about 100 FPS at 1800 draw calls, about 90 at 2000, about 150 at 900, and about 230
in the menus. There is no frame limit by default (`ARKHAM_FPS_CAP=120` sets one). It was 48 FPS with one guest thread at a time. In a 2000-draw
frame of 10.7 ms the engine takes 4.2 ms, the Direct3D thread needs 4 ms more after that and the OpenGL thread 2.3 ms more, while the GPU sits
near 60%. FPS stays close to 120 between about 1050 and 1400 draws without a limit being set; the cause isn't known. What got it here:
- Guest threads run in parallel. Non-stack loads and stores use load-acquire and store-release, because x86 keeps them in order and the
  game depends on it: built with plain accesses, the render thread reads garbage from its command ring at the first level load.
- `runtime/d3d_queue.c` moves Feral's Direct3D 9 to OpenGL layer to its own thread, because it took half the render thread's time. The engine
  reaches it through COM interfaces, so their vtable slots are patched at startup. A method not listed there runs synchronously. Repeated
  `SetRenderState` and `SetSamplerState` calls, 60% of the traffic, are dropped before the queue. The frame now goes game thread, render thread,
  Direct3D thread, OpenGL thread. The engine still waits each frame for occlusion results, which makes the chain finish the previous frame:
  `ARKHAM_TRACE_PIPE=1` shows that wait, about half the frame, and it is the largest remaining reserve. `ARKHAM_D3D_DIRECT=1` turns the thread off.
- `runtime/gl_queue.c` moves OpenGL to a worker thread. A function not listed there runs synchronously, so a missing entry costs speed only.
  Occlusion query results must stay synchronous: read ahead of the commands still in the ring, they sometimes come back as 0 samples, and a
  wall vanishes for one frame (`ARKHAM_SNAP_POPS=dir` catches that, in a still scene). `ARKHAM_GLQ_SYNC=all` or a list of names finds such calls.
- Mutexes and condition variables sleep on a bucket per address instead of polling one global event.

Tried with no measurable gain: a fixed guest memory address, inlined `pthread_getspecific`, `memcpy` and similar calls, not saving
registers around simple shims, per-site indirect call lookup, ordering hot functions at link time. Apple's multithreaded OpenGL engine
also changes nothing, because it runs each draw on the calling thread, so its `CGLEnable` is ignored.

Not working or not checked yet:
- Stalls when doors open and new areas appear, up to 268 ms in play. In the reports each first draw with a new shader takes 8-9 ms inside OpenGL
  (27 draws took 236 ms in one stall), and the Direct3D thread also waits while OpenGL creates programs (`glProgramStringARB`, about 1.3 ms each).
- Choosing the old "Arkham Mansion" save (marked OLD, no date) crashes in `F_02406f0`, on a null pointer in the save summary.
- Steam callbacks aren't delivered, so achievements and stats don't work.
- Gamepads: IOHID is stubbed.
- `ARKHAM_STUB_ALL=1` still makes imports without a shim return 0.

Test tools: `ARKHAM_INPUT="40:key:36,45:down:13,60:up:13,70:move:900:500"` scripts input, replaces the cursor position and blocks
writes to Steam Cloud. `ARKHAM_GLSNAP=dir` saves frames from the back buffer, `ARKHAM_FPS=1` shows a frame counter (also logged as
`[fps]`), `ARKHAM_TRACE_EH` prints guest C++ throws, `ARKHAM_TRACE_HEAP` a memory profile and `ARKHAM_TRACE_AUDIO` CoreAudio calls.
`ARKHAM_TRACE_IMPORTS` shows calls per second of each import, `ARKHAM_TRACE_GLQ` what makes the render thread wait for OpenGL, and
`ARKHAM_TRACE_UNALIGNED` which code traps on values crossing a 16-byte boundary (add such byte-stream code to `UNALIGNED_FUNCTIONS` in
`tools/lift.py`). `ARKHAM_GL_DIRECT=1` runs OpenGL on the calling thread. `ARKHAM_SERIAL=1` runs one guest thread at a time, for telling
threading bugs from others, and needs a binary from `tools/build.py --serial` (in `build/serial`). `tools/profile.py` samples the running game.
`ARKHAM_TRACE_HITCH=1`, on in `tools/run.sh`, logs `[hitch]` reports: which OpenGL calls were slow and what each thread was doing when a frame
came more than 40 ms late. `tools/hitches.py` prints the longest ones. `tools/walk.sh` goes into the first level in about a minute, skipping the
movies with clicks, then follows a route. Past the lift the route needs someone steering, so warn first: the game window covers the chat.

## Next

1. Door stalls: save the shader programs to disk and build them ahead of time on a background thread with a shared OpenGL context. Check first
   whether that reaches the 8-9 ms part, which happens at the first draw and not when the program is created.
2. The wait for occlusion results at the start of each frame, about half the frame. Answering "visible" without waiting nearly doubles the draw
   calls (3700 instead of 1950), so the engine would have to read results one frame later. That means changing engine logic in the translated code.
3. Steam callbacks, gamepads, and then iPad and iPhone.
