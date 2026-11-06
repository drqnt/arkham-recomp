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
tools/build_shader_cache.sh  # once, with the game closed: builds pixel shaders ahead of play, about 12 minutes
python3 -I tests/difftest.py 400   # instruction semantics against real x86 under Rosetta
```

## Layout

- `tools/lift.py`, `emit.py`, `x86.py`, `macho.py`: the recompiler. `gen_gl.py`: OpenGL shims from SDK headers.
- `tools/showcode.py`, `xref.py`, `vtable.py`, `objcdump.py`, `hook_callees.py`, `config/hooks.json`: reverse-engineering aids.
- `tools/try.sh`: rebuild and run briefly with a compact summary.
- `runtime/rt.c`, `rt.h`, `rt_ops.h`: guest memory, dispatch, thread gate, SSE and x87 semantics.
- `runtime/shims_*.c`: libc, threads and GCD, GNU libstdc++, CoreFoundation, Objective-C bridge, OpenGL and CGL, Steam.
- `runtime/d3d_queue.c`, `gl_queue.c`: the Direct3D and OpenGL threads. `pipelines.c`, `tools/warm_shaders.c`: building shaders ahead of play.

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

Anti-aliasing: 4x at 2560x1600 used to take the first level from about 92 FPS to 52 at 1600-2000 draws, while 2x cost only about 10%. The level is set
in two places, `FSAALevel` in the game's Preferences Data and `MaxMultisamples` in MacInit/BmEngine.ini, and changing one alone does nothing.
- A whole-screen stencil clear in the middle of a render pass makes the GPU load and store every tile of the target: 1.6 ms at 4x, against 0.05 ms as
  the first thing in a pass. The game does one at the end of each shadowed light's pass, so `gl_queue.c` holds a stencil clear back until a draw is going
  to read stencil, or drops it when a later clear covers it. Shadows look the same (`ARKHAM_STENCIL_NOW=1` is the old way). Re-attaching the same depth
  buffer between draws, 59 times a frame, turned out to cost almost nothing.
- Feral's layer releases the OpenGL context and takes it back about 2500 times a frame. The worker now keeps it (`ARKHAM_CTX_RELEASE=1` is the old way).
- Together: 76 FPS in place of 53, 78 in place of 52 at 1600-2000 draws, and 71 in place of 55 at 2000-2400. The speeds without anti-aliasing above were
  measured before these changes. The Windows version in Parallels on this machine is limited by the CPU: about 75 FPS on 4 virtual cores and 82-85 on 8,
  the same with 4x, without it and at 720p, so it isn't known whether its 4x works.

Shaders: Apple's OpenGL builds a Metal function the first time a draw needs it, 10-16 ms each, one at a time per process, and keeps the result on disk.
That is the stall when a new area appears. Feral's layer translates a Direct3D shader when it is created, from its bytecode alone, so
`tools/build_shader_cache.sh` runs all 31,000 shaders from the game's caches through it (25 seconds, `ARKHAM_SHADER_EXTRACT`), and `tools/warm_shaders.c`
draws each pixel shader once, with no pixels touched, in the states it is likely to be used in, chosen from its parameter names. A cache entry depends
on the program text, the context's "no recovery" attribute, a few settings Feral's layer makes, blending, color mask, target format and sample count, but
not on the vertex program. In the first level it covers 91% of pixel shader builds. The result is 795 MB in a Metal cache folder of the game's own,
through the bundle identifier in `runtime/Info.plist`. It halved the time spent on slow draws on a first pass, 5.9 s to 2.9 s in the first 110 seconds.
Vertex shaders are most of the rest: they also depend on the vertex layout and on what the pixel shader reads, so they are still built in play, once
each. The build has to be redone after changing the anti-aliasing level, and `tools/run.sh` says so. Metal gives a process that starts during another's
write an empty cache file of its own that later runs never see, so the game and the build can't run together, and files from parallel builds can't be
merged by appending: the entries are there but don't hit.

Sound: the mixer's thread doesn't wait for the runtime's global lock any more, which every shim not on the list in `rt_shim_no_gate` takes. A slow call
used to be heard as a gap. `[audio] gap` lines in the log report frames the system asked for that weren't ready.

Tried with no measurable gain: a fixed guest memory address, inlined `pthread_getspecific`, `memcpy` and similar calls, not saving
registers around simple shims, per-site indirect call lookup, ordering hot functions at link time. Apple's multithreaded OpenGL engine
also changes nothing, because it runs each draw on the calling thread, so its `CGLEnable` is ignored.

Not working or not checked yet:
- Stalls when doors open and new areas appear were up to 268 ms in play. The pixel shader build above should shorten them, but it hasn't been tried at
  the doors yet. The start of the level still has one frame of 215-370 ms, and some runs have frames of about 60 ms whose cause isn't known.
- Choosing the old "Arkham Mansion" save (marked OLD, no date) stops in `F_02406f0`, on a null pointer in the save summary. A save that the Windows version
  wrote does the same: the two versions share Steam Cloud, and playing the Windows version replaced Save2.sgd. It may be that the old save is one too.
- Cmd+Q has only been tried by script, which leaves its dialog open. Holding Cmd used to crash at the next key-up, because the game's event monitor block
  reached the system as nothing, and it now goes through `event_monitor_send`.
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
Its `SLOT` picks the save, and `FS=1` adds `ARKHAM_TRACE_FS`, whose log lines distort timing. `ARKHAM_TRACE_PIPES=1`, on in `tools/run.sh`, logs the state of
each draw over 1 ms as `[pipe]`, and `ARKHAM_DUMP_PROGRAMS=dir` saves those programs. `ARKHAM_TRACE_FRAME=seconds` writes out two frames as `[fmap]`, pass by
pass, with the GPU time of each: it waits for the GPU after every pass, so only compare those frames with each other. `ARKHAM_TRACE_GATE=1` reports what holds the
runtime's lock for over 2 ms. Judge a run by its worst frames and `[audio] gap` lines too, not only FPS. `ARKHAM_GLSNAP` stalls the game every second or so,
sound aside, so don't measure with it on.

## Next

1. Try the shader build at the doors in play. If stalls remain, vertex shaders are next: find out how many of their variants can be predicted.
2. The wait for occlusion results at the start of each frame, about half the frame. Answering "visible" without waiting nearly doubles the draw
   calls (3700 instead of 1950), so the engine would have to read results one frame later. That means changing engine logic in the translated code.
3. A Direct3D 9 layer on Metal in place of Feral's layer and Apple's OpenGL. The engine reaches graphics through COM interfaces that are already patched,
   iPad and iPhone have no desktop OpenGL so they need this anyway, and pipelines could be built in parallel. It is a large piece of work.
4. Steam callbacks, gamepads, and then iPad and iPhone.
