# Processor time: the game thread and the render thread (branch cpu-speed, 2026-11-08)

The owner asked for the heaviest thread to be made faster, on a branch of its own, measured on the Mac first and then taken to the phone,
where at the limit of 120 the game thread (1dab9d0) took about 7 ms a frame and the render thread (1c260f0) about 8.6 ms, waiting included.

## How it was measured

- Route: the capsule route (`ARKHAM_REPLAY=logs/saves/named/capsule_route.txt tools/walk.sh 170 ""`), with the GPU's work taken off
  (`ARKHAM_MTL_SAMPLES=1 ARKHAM_MTL_CHEAP_PS=1`), so that the processor is the limit. The first runs also had `ARKHAM_MTL_NOPRESENT=1`;
  that leaves the owner's display black for three minutes in full screen, so later runs present the (grey) picture. Presenting adds a little
  waiting to the game thread's time, which makes the later numbers slightly pessimistic against the first ones.
- Number: each guest thread's CPU time a frame, from the `[fps]` lines, over 119 s of play and over its parts. The part 69-92 s into play is
  where the game thread is the limit (98% busy), so it shows the game thread's changes best. Runs of the same build differ by about 2%.
- Where the time goes: `tools/profile.py` (the system's `sample`), and Instruments' CPU Counters (`xctrace record --template 'CPU Counters'
  --attach pid`), read per thread from the `CounterMetricByThread` table.

## What the processor does

CPU Counters in play, share of issue slots: the game thread 22% useful, 54% held up in the back end (waiting on data and dependencies), 15%
in instruction delivery, 8% discarded after mispredicted branches; the render thread 24%, 58%, 10%, 8%. So the translated code is bound by
memory and dependency latency rather than by the number of instructions: a 14% cut in instructions gave 5% in time.

At the start the game thread spent 24% of its samples inside imports (`rt_imp`), not in translated code.

## Changes, with their effect

| Change | Game thread, ms a frame (part 69-92 s) | FPS there |
|---|---|---|
| Start (no presenting) | 2.73 | 351 |
| Guest memory as a restrict parameter of its own (`F_xxx(CPU *restrict c, u8 *restrict M)`) | 2.59 | 377 |
| Objective-C bridge: plans per class and selector | 2.56 | 381 |
| (presenting from here) inlined imports, also at a jump; heap thread caches; HFS URLs kept; 4096 sleep buckets | 2.54 | 386 |
| Indirect calls keep their last target at the site | 2.52 | 388 |

Over the whole of play, presenting included at the end: 309 to 324 FPS, the game thread 2.84 to 2.71 ms, the render thread 2.89 to 2.84 ms.
Inside the game thread the share in imports went from 24% to 14%, the dispatcher's from about 4% to 2%.

- **Memory as a parameter.** With guest memory at a constant address the compiler had to assume a guest store could change the emulated
  registers in the CPU struct, so every function loaded all eight at entry and stored all eight back at each call and return. One shard:
  179k instructions to 154k; the binary 262 MB to 237 MB. Calls still make the compiler reload what it must: LLVM treats a call as able to
  change memory behind a pointer argument even when the pointer isn't passed to it (checked in `F_1da88c0`, which reads posix_memalign's
  answer after the call).
- **Objective-C.** Feral's code sends about 106,000 messages a second in play, some 310 a frame, nearly all asking the window and the
  screen for their geometry. A plan keeps the method, the argument kinds and the return type for a class and a selector in the game's image;
  special cases always take the slow way (`ARKHAM_OBJC_SLOW=1` sends everything that way). 5.3% of the game thread to 3.4%; most of the
  rest is AppKit's own work. `ARKHAM_TRACE_OBJC_COUNTS=1` reports the busiest selectors and where they come from.
- **Inlined imports.** `ARKHAM_TRACE_IMPORTS=1 ARKHAM_TRACE_IMPORT_FROM=_wcscmp,...` showed wcscmp 3.4 million times a second from one
  Feral wrapper that ends in a jump to it, tolower 570,000, pthread_self 250,000, isnanf, isinff, sinf, cosf, fmodf, malloc_size,
  strcasecmp. The lifter now writes these in place, as the shims do them, and a jump to such an import runs it the same way and returns.
- **HFS paths.** The Scaleform interface looks for its localization files (`GFxUI.RUS`, `GFxUI.INT`) about a dozen times a second
  through HFS paths, and each conversion to a URL asked CarbonCore about the volume: 3-5% of the game thread. The URL for a path is kept.
  `ARKHAM_TRACE_URLS=1` reports the paths made into URLs most often.
- **Heap.** Over a million allocations a second from several threads, each under one lock. Each thread keeps up to 32 freed blocks of each
  class up to 1 KB. posix_memalign left the list of costly calls; no change in the totals. `ARKHAM_HEAP_SHARED=1` turns it off.
- **Sleep buckets.** Threads asleep on a mutex or a condition shared 64 buckets, so a hot mutex's unlock often found someone else's sleeper
  in its bucket and broadcast for nothing. 4096 now. Within the noise.
- **Call sites.** A call through a pointer keeps the dispatcher table's entry of its last target and calls it straight when the target is
  the same: an indirect branch per site, which the processor predicts, and no search of the 16 MB table (`rt_dispatch_ic`). About 1-2% in
  both threads; the binary grows to 268 MB.

## Tried or worked out, and left

- **Unordered memory** (`tools/build.py --define ARKHAM_PLAIN_MEMORY`): the game stops at the start in Feral's Direct3D layer (a lock-free
  structure seen out of order, `0xc0ffee` sentinels), so x86's ordering can't be dropped as a whole. A microbenchmark on this Mac puts
  load-acquire and store-release at about 2-3 times plain loads and stores in a tight loop of field accesses, and a store followed by a
  reload at +20%.
- **The function's own locals in registers.** `frame_window()` in tools/lift.py finds functions whose locals only their own ebp-based
  accesses reach. Under strict rules (no landing pads, no address taken, no jump tables) they hold 4.5% of the local accesses in the code,
  and about 22% of the samples among the 60 hottest functions of the game thread; landing pads would add about as much again, at the cost
  of writing the locals back before each call. A pointer of its own for the frame wouldn't keep them across calls (LLVM's rule above);
  real promotion to C variables would. Estimated at about 5% of the game thread; not done.
- **64-bit addresses for base plus offset** (the offset folded into the load, the base computed once): 2.7% fewer instructions in a shard.
- **Hot functions first at link time** (`tools/build.py --order build/hot.order`, from the samples): no change.
- **Quiet runs** (windowed, behind other windows, not pausing in the background): the game stalls at the menu when started windowed, and
  it writes Feral's preferences back by a way the file shims don't see, so that was taken out; the owner's preferences were put back byte
  for byte.

## Still open

- The render thread spends about 20% in the Direct3D queue's machinery: per second about 6.5 million repeated state calls are skipped,
  2 million go to the queue and 30,000 wait for an answer (nearly all occlusion queries). Each skip still goes through the whole call.
- The game thread's remaining imports: the Objective-C sends' AppKit work (3%), a `dispatch_sync` to the main thread every frame (1.5%),
  CFString work in Feral's file layer (1.5%), `stat` (0.7%).
- Megamorphic call sites such as UnrealScript's native table change their kept target all the time; a small table of recent targets
  could serve them.
- The phone: the same build is ready (`ios/app/run.sh "" build-only` links), not yet installed or measured there.

## Tools added

`ARKHAM_TRACE_OBJC_COUNTS`, `ARKHAM_TRACE_IMPORT_FROM`, `ARKHAM_TRACE_URLS`, `ARKHAM_OBJC_SLOW`, `ARKHAM_HEAP_SHARED`; guest threads named
after their start function (`guest 1dab9d0`); `NOBUILD=1` for `tools/go.sh`; `tools/build.py --order`; `frame:*` counts in the lift report.
