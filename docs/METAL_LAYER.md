# A Direct3D 9 layer on Metal

`ARKHAM_RENDERER=metal tools/run.sh` runs the game on it. OpenGL through Feral's layer is still the default until Metal has been played by hand.

Why: iPhone and iPad have no desktop OpenGL, so the game can't draw there through Feral's layer. On the Mac it also turned out much faster, see below.

## What the game uses

From `tools/shader_census.py` over the game's shader caches, and `ARKHAM_D3D_CENSUS=file` over a run through logos, menus and the first level. Other places may add some.

- Shaders: Shader Model 3 only. 29,869 pixel shaders, longest 325 instructions, and 1,593 vertex shaders, longest 250. No loops, no calls, no predicates, 203 `ifc` in all.
  Samplers are 2D and cube. Source modifiers are negate and abs. Relative addressing is on vertex constants only. Pixel inputs are texcoord, color and vPos.
- No fixed function at all: no FVF, transforms, lights or texture stages. `SRGBWRITE` is always 0.
- Draws: indexed triangle lists, strips and lists from memory. Streams 0-2, frequency always 1 so far, though hardware instancing is there in case. Stride-0 streams become constant attributes.
- Texture formats: DXT1, DXT5 (DXT3 is there in case), A8R8G8B8, L8, and for targets A16B16G16R16F, G16R16F, R32F, A16B16G16R16 and D24X8, which is the shadow map. Depth surfaces are D24S8, 1x and 4x.
  Render targets are A16B16G16R16F and A8R8G8B8 at 4x. Cube textures exist, volume textures don't.
- Buffers: all managed, locked once when created. Anything dynamic is drawn from memory.
- Queries: occlusion (168 a frame) and event.
- OpenGL outside Feral's layer: only at startup, so movies and UI go through Direct3D.

## How it fits in

- `runtime/d3d_shader.c` translates bytecode to Metal Shading Language. `tools/check_shaders.py 0` runs all 31,462 shaders through it and the Metal compiler.
- `runtime/mtl_device.m` is the device. `rt_mtl_method` gives `d3d_queue.c` an implementation for a vtable slot, and that runs on the Direct3D thread. No OpenGL worker is involved.
- Objects are small guest allocations with the same vtable pointer as Feral's and an index to a host object.
- Feral's `IDirect3D9` and `CreateDevice` still run, and make an OpenGL context that draws nothing. Their code calls itself through the same vtables with its own objects, so a call whose
  return address is inside the layer's code goes to Feral's implementation. A method of a Metal object with no implementation returns an error and logs its name once as `[mtl]`.
  A device method left to Feral's layer logs its name once too: `GetCreationParameters` and `TestCooperativeLevel` are the only two so far. iOS has to replace this part.
- The frame goes to a `CAMetalLayer` in a view on top of the window's content.

Same clip space and texture origin as Direct3D, so no flips. Direct3D 9 puts a pixel's center at a whole coordinate and Metal half a pixel further on, so positions move by +0.5 pixel.
Getting the sign wrong moves the picture a whole pixel, leaves the right and bottom edges unwritten, and the ambient occlusion blur turns that into a dark band.

## What makes it fast

At 4x a pass that touches little still costs 0.55 ms, because it loads and stores a 2560x1600 multisampled target. So the work went into not having those passes.

- Shadow maps go ahead of the scene. For each shadowed light the engine moves from the scene to a 512x512 shadow map (0.015 ms) and back (0.58 ms), nine times a frame in the first level.
  The scene's encoder now stays open while the shadow pass is recorded in a command buffer that is committed first. Each such pass draws into its own copy of the shadow map, so a light's draws
  in the scene read what was made for that light. `[mtl]` says so if a pass sent ahead reads something the scene wrote this frame. `ARKHAM_MTL_NO_AHEAD=1` turns it off.
- Targets bind lazily, so setting a target and setting it back with no draws costs nothing.
- Resolves happen as the pass ends. A pass on a surface is told where that surface was resolved to last time, and the resolve costs nothing extra when it comes again.
- After the translucency the engine sets another target, clears only depth and stencil, sets the scene back and averages it. Done at the switch, that clear ended the
  scene's pass, and the average needed a pass of its own that loaded the whole 4x target again, about 0.55 ms a frame. A clear of only depth and stencil now waits across a
  switch to a target of the same size, drawn into that target's pass if it's open, so the average happens as the scene's pass ends. In Instruments that took the GPU's fragment
  work from 5.7 to 5.1 ms a frame in the first level.
- Clears: a clear at the start of a pass is a load action. A clear of one attachment in the middle of a pass is a draw. A stencil-only clear covers only the box stencil was written to since
  the last clear, and is skipped when that's empty.
- Occlusion answers don't wait for the frame. The command buffer goes to the GPU as soon as a pass with queries ends, so the engine's wait at the start of the next frame drops from about 6 ms to
  about 0.2, with real answers. `ARKHAM_MTL_LATE_QUERIES=1` turns it off. Answering without waiting nearly doubles the draws, as the README says.
- Presenting has a command buffer of its own, so nothing the engine waits for waits for the display.

Measured in the first level, 2560x1600, 4x, M5 Pro, walking the scripted route with shaders already compiled:

| draws a frame | OpenGL FPS | Metal FPS | Metal without the display's limit |
|---|---|---|---|
| 600-1200 | | 120 | 198 |
| 1600-2000 | 78 | 120 | 154 |
| 2000-2400 | 71 | 120 | 146 |

The worst frame in a typical second is 8.6 ms against about 14, one frame over 40 ms in 100 s of walking, and no sound gaps. The Metal column stops at the display's 120 Hz because the Direct3D thread
waited for a free drawable. `ARKHAM_MTL_NOPRESENT=1` doesn't show anything, which is how the last column was measured.

Another thread now does that waiting, so the game isn't held to the display: a frame that finds a drawable ready goes on screen, and any other frame is not shown. `ARKHAM_MTL_VSYNC=1` brings the
wait back. The screen still shows at most its own rate, so frames above it can only shorten the time from input to picture. The end of each `[fps]` line says what really reached the screen: how many
a second, how many were dropped, the longest gap between two, and how long after presenting they arrived.

On the same route that gives 175 FPS at 600-1200 draws, 173 at 1200-1600, 151 at 1600-2000 and 138 at 2000-2400, and 240 in the menu. The screen gets 120 a second, with 8.3 ms between frames in a
typical second, and there were no sound gaps. A frame arrives about 21 ms after presenting, against 12-13 ms during the logos, when frames come too slowly to queue. The layer keeps 3 drawables,
so 2 may shorten that, and whether it costs smoothness hasn't been measured in the game.

### Where the GPU's time goes

Instruments over 5 s of the first level at 2560x1600 4x, about 180 FPS, after the change above: fragment work 5.1 ms a frame and busy 93% of the time, vertex work 1.2 ms. So the
GPU is the limit, and of its two halves the fragment side. A frame's fragment time:

| pass | ms |
|---|---|
| lights, 260 draws with 60 queries | about 0.85 |
| three passes on the scene after the lights, 1 to 4 draws each | about 1.7 |
| depth first, 300 draws | 0.63 |
| base pass, 430 draws | 0.62 |
| a 4x mask target, 2 draws | 0.52 |
| eight half-size passes | 0.28 |
| bloom, shadow maps, the final picture | about 0.5 |

The three short passes cost 0.5 to 0.7 ms each for 1 to 4 draws: that's loading and storing the 4x color and depth. Each ends because the engine averages the scene and then
samples that texture in the next draws, for refraction, so they can't be one pass on a tile-based GPU. What's left to try there is not loading or storing depth in passes
that don't use it.

With fewer samples, `ARKHAM_MTL_SAMPLES=n` on the same route (the engine is still told 4x): 4x 159 FPS and 6.4 ms of GPU a frame, 2x 168 FPS and 4.55 ms, none 196 FPS and 4.4 ms.
Without multisampling the GPU could give about 227 FPS but the game stops near 200, because the CPU is the limit too: in Instruments the game thread runs 94% of the time,
its time spread thinly over translated code with no one hot function. About 7% of it goes to file path calls (`CFURLCreateWithFileSystemPath`, `stat`) from Feral's file layer
every frame and about 4% to the Objective-C bridge. The render thread spends about 9% waiting for Direct3D calls that return something. The Windows version on a Ryzen 7
5800X and an RTX 3060 Ti gives about 350 FPS at 2560x1440 4x, native code on both sides.

## Shader compiling

Text to library takes about 40 ms a shader in the game, and a pipeline about 10 ms, on the Direct3D thread at the first draw that needs them. Metal keeps both on disk, so the second time costs nothing:
487 shaders in 13 ms. Without help a new place stalls for up to 2.4 s, 44 shaders and 59 pipelines in the worst case seen.

`tools/prebuild_shaders.sh` fills the cache ahead of play, about 2.5 minutes and 350 MB, again whenever the executable's path or its Info.plist changes. It has to be the game's own binary that does it, `build/arkham --prebuild-shaders list`: entries depend on
the executable, so another program with the same bundle identifier writes to the same file and the game never finds them. Renaming a copy of a program is enough to miss. Rebuilding the game at the same
path keeps its entries. The first such build found 2009 of the 31,462 shaders already there, which is what the game had compiled itself, and that's the check that the key is the same.

A pixel shader can read an output its vertex shader doesn't write, which Metal won't link. Such a vertex shader gets a second translation that writes all ten outputs, the missing ones as
(0, 0, 0, 1), one for any pixel shader, and the prebuild compiles that too (`vsall_` files). Before, each combination had its own translation, which the prebuild couldn't know: those stayed
in Metal's cache from earlier play at the same path, so moving the game into `Arkham.app` brought back 10 to 14 s of stalls in an 11-minute game, at places played many times before.
Each shader that does compile in play is logged as `[mtl] vs_... compiled in N ms, not from the cache`, and the `[fps]` line counts them.

**Apple's Metal HUD changes the cache's key.** With `MTL_HUD_ENABLED=1` the running game finds none of what the prebuild compiled: 0 of 30 in a test, 38 ms each, against 30 of 30 in
1 ms in all without it, and 405 compiles in a 3-minute run. The Metal device is the same class either way, so it's in how Metal compiles, which Apple doesn't document, and the
HUD's key is the same from run to run. The headless prebuild can't reach that side, so when `config.local` turns the HUD on, `tools/prebuild_shaders.sh` also starts the game
with `ARKHAM_PREBUILD_IN_GAME`, which compiles the list behind the first logo once the window is up, about 2 minutes, and quits. After that the game with the HUD found 30 of 30.
The HUD's own drawing costs about 0.3 ms of GPU a frame besides. `ARKHAM_COMPILE_TEST=list` makes the game compile a list of files once its window is up, log each time and
quit, which is how this was found.

Pipelines depend on state and are only built when first seen, at about 10 ms, and Metal keeps those too. On iOS the libraries will have to be built on the Mac, because a compile on the phone
costs more and the source has to be there.

## Depth bias

The game's values aren't depth in the sense the Direct3D documentation gives. Feral's layer calls `glPolygonOffset(slope, bias * 500000)`, seen by logging it, and the values were chosen with that in
mind: Batman's cowl gets about -0.008 and his cape about -0.025. Used as plain depth, 34 times stronger, the cape covers the cowl at the neck, in a dark wedge that ambient occlusion makes darker.
Metal measures bias in steps of the depth format, which for a float format halve with each halving of depth below 0.5. Measured, and the same as what OpenGL has on this hardware, as long as Apple's
OpenGL uses a float depth format underneath, which hasn't been confirmed.

## Full-screen effects

The game's material effects are one full-screen strip through `DrawPrimitive`, at depth 0, with a viewport of -1,-1,2561,1601. That's the purple of the scanner at the start of the game and the lights going out in the elevator about 6 minutes in. Direct3D declares the origin as unsigned and OpenGL takes it as signed, so read as unsigned it put the whole strip about four billion pixels away, with no error anywhere.

## Found by reading the code

After those, the layer was read through for mistakes of the same kind, that go wrong with no error anywhere. Fixed, though none of them had been seen in play:

- A constant a shader defines itself has to win over the engine's even when read through `a0`: SpeedTree leaves and terrain do that. Such reads go through a `crel()` that knows the shader's own values.
- `SetStreamSourceFreq` was ignored, so instanced draws would have drawn one instance with the wrong data.
- A frame that filled its 48 MB of upload space started again from the front, over data the GPU hadn't read yet. It now goes on in another buffer.
- The pipeline cache was keyed on objects' addresses, which come back for the next object once one is freed, so a new shader could get an old one's pipeline. Keys are serials now.
- Freed object ids were kept for only 1024 at a time, and the table could run over.
- DXT3 textures had no format, and `Clear` read the viewport's origin as unsigned, like the strip above.

## Tools

- `ARKHAM_TRACE_FRAME=seconds`: one frame after that long, with each pass alone on the GPU, as `[fmap]` lines. `ARKHAM_TRACE_BUSY=frames` does the same that many frames after the first with over 300 draws.
- `ARKHAM_FRAME_PNG=dir` with it: the target after each pass of that frame, as pictures.
- `ARKHAM_TRACE_DRAWS=dir` with it and `ARKHAM_MTL_NO_AHEAD=1`: every draw's state as `[draw]` lines, and a crop of color and alpha after each draw that changed it. `ARKHAM_TRACE_CROP=x,y,w,h` picks the place, `ARKHAM_TRACE_DRAWS_FROM=n` skips the pictures before that draw, and an empty directory only logs. Shader names are file names in `logs/msl`.
- `ARKHAM_TRACE_KEY=1`: the = key starts such a trace. `ARKHAM_TRACE_LEVEL=seconds` does it that long after a replayed level has started.
- `ARKHAM_D3D_TRACE=from,step,count`: every call the engine makes to Direct3D during one frame, as `[d3d]` lines, in front of either renderer, so two runs show whether the engine asks for the same thing. With it, `ARKHAM_MTLSNAP` and `ARKHAM_GLSNAP` save only those frames.
- `ARKHAM_RECORD=file` with `tools/walk.sh`: the keys pressed once a level has started. `ARKHAM_REPLAY=file` presses them again, counting from the first frame with over 300 draws, because loading times differ. A scripted run ignores the real mouse, so a route has to be walked with keys. Stalls early in a replay leave Batman behind his route, so keep traces to the part being looked at.
- A scripted run reads `logs/saves/frozen/SaveN.sgd` if it exists, and a played game keeps each save in `logs/saves/history`.
- `ARKHAM_MTL_LABELS=1` names every pass, and Instruments' Metal System Trace then shows what each costs in a normal frame, unlike `[fmap]`, where each pass waits alone:
  `xcrun xctrace record --template 'Metal System Trace' --attach <pid> --time-limit 5s --output file.trace` with the full Xcode as `DEVELOPER_DIR`, then `xctrace export` of the
  `metal-gpu-intervals` table.
- `ARKHAM_TRACE_RESOLVE=1`: why an average of a 4x target needed a pass of its own.
- `ARKHAM_MTL_SAMPLES=n` and `ARKHAM_MTL_CHEAP_PS=1` (every pixel shader one gray color) take work off the GPU, so a run shows the CPU side's limit. The `[fps]` line ends with
  the three busiest guest threads' CPU time a frame and the address each started at: 1c260f0 is the engine's render thread, 1dab9d0 its game thread. Time spent spinning
  while waiting counts as CPU time there.
- `ARKHAM_TRACE_SLEEP=1`: who in the game sleeps, how often and how long, every 5 s.
- `tools/build.py --define ARKHAM_PLAIN_MEMORY` builds the translated code with unordered memory into objects of its own, only to measure what x86 ordering costs.

On the capsule route with multisampling off and one-color pixels, the game gives 238 FPS and stops at 240, with the render thread polling Feral's wait for the GPU
(F_040a930, usleep(10) about 26,000 times a second). With `ARKHAM_MTL_NOPRESENT=1` the same run gives 283 FPS, peaks of 338 and no polling: the cap comes from presenting.
The likely cause: the copy into a drawable waits on the GPU until the display lets the drawable go, and the queue runs in order, so everything after it waits too. Without
anything presented, the render thread is the limit, about 3.5 ms of CPU a frame. Unordered memory changed nothing measurable in that test.
- `ARKHAM_FPS_CAP=n`: a frame limit, as on OpenGL.
- `ARKHAM_MTLSNAP=dir`: the back buffer at frames 30, 120 and every 600th. It stalls those frames.
- `MTL_DEBUG_LAYER=1 MTL_DEBUG_LAYER_ERROR_MODE=nslog MTL_DEBUG_LAYER_WARNING_MODE=ignore`: Metal's own checking. Clean so far.

## Stages

1. Shader translator. Done, all shaders compile.
2. Device, with logos, menus and the first level. Done.
3. Queries, resolves, clip planes, shadows, sRGB textures. Done, and the first level matches OpenGL by eye, but only that part of the game has been seen.
4. Play it by hand through more of the game. Started: about 10 minutes so far, past the first level, with a clean exit. `Device.Reset`, which a change of resolution calls, has never run, and reading back a render target isn't written.
5. iOS: device creation without OpenGL, UIKit shell, input, game files on the device.
