# Porting to iPhone and iPad

The target is the user's iPhone (iPhone19,4, A20 Pro, 12 GB, iOS 27.2), then an iPad Pro M1. Personal use only, as for the Mac, installed from Xcode.

## What is known already

`ios/probe` measured the phone without any special entitlements (`logs/probe_*.txt`):

- The guest's 4 GB window fits at 0x7000000000, with about 2 GB of address space to spare, and an app may use 3.2 to 3.5 GB of memory.
- 336 MB of code loads, unaligned accesses that the translated code makes work, and one core is as fast as one of the Mac's.
- BC (DXT) textures, which all of the game's textures are, are supported.
- 12 whole-screen 4x passes take 4.5 ms against 2.6 on the Mac, in a short burst. Heat under a long load isn't measured.

On the Mac, the game uses 1.94 GB (`footprint`) in the first level at 2560x1600 with 4x: about 1.07 GB of it graphics, 440 MB the game's own memory, the rest system
libraries and caches. A phone's screen and its render targets are smaller, so this fits, though later levels haven't been measured.

## What carries over

- The translated game code (`gen/`), and the translator: it is plain C, so it compiles for iOS as it is.
- The Direct3D 9 layer on Metal, the shader translator, FXAA, presenting through its own queue, the Direct3D call queue.
- Bink and FMOD, which are translated guest code.
- DualSense and Xbox controllers through GameController, which iOS has too.

## What has to change, in order

1. **Build.** Runtime and shards for `arm64-apple-ios`, linked into an app with its Xcode project. The runtime's AppKit, Carbon, CoreServices and OpenGL parts don't
   exist there, so they move behind a platform split.
2. **Start without OpenGL and AppKit.** Feral's `IDirect3D9::CreateDevice` makes an OpenGL context, and its Cocoa layer runs `NSApplication`, a window, the screen,
   events, menus and the launcher's web view. The launcher goes; device creation becomes the Metal layer's own; the rest gets the smallest stand-ins the game needs,
   in a UIKit shell with a `CAMetalLayer` view.
3. **Game files.** About 7 GB copied into the app's Documents through Finder, and the paths mapped there.
4. **Steam.** iOS has no Steam client, and the game asks Steam whether the copy is owned. See the decision below.
5. **Sound.** FMOD's output goes from the Mac's output unit to RemoteIO, with an audio session.
6. **Shaders.** On the phone a compile costs more and must not happen in play, so the Metal libraries are built on the Mac into a `.metallib` that ships in the
   app. That needs Xcode's Metal Toolchain, a separate download.
7. **Frame rate and heat.** 60 FPS by default with a 120 option later, as the user wants; 2x MSAA with FXAA, a lower internal resolution, and MetalFX upscaling.
8. **Touch controls**, after the game runs with a controller.

## Decisions

- **Apple developer account:** the free Personal Team (user's decision, 2026-11-07). The app is installed again from Xcode every 7 days, and it has to stay within the
  memory an app gets without the larger-limit entitlement, 3.2 to 3.5 GB on this phone.
- **Metal Toolchain:** downloaded (user's decision).
- **Steam:** checked once, on the Mac, when the game is installed on the phone. The user plays where the power and the internet are often off, so nothing may need
  the network while playing. At install a small tool on the Mac asks the real Steam client, through Valve's own library, whether the account owns the game; this
  works in Steam's offline mode too. The answer goes onto the phone with the game, and from then on the phone asks nobody, the way Steam's offline mode lets a bought
  game start. Achievements, stats and saves stay on the phone. The user would rather have no check at all; I won't write one that makes the answer up instead of asking
  Steam, and this way costs them nothing they don't already do. A refresh through AltStore keeps the app's data, so the answer stays.
- **Reinstalling every 7 days** with the free account, through Xcode or AltStore, is the user's plan. Either needs the Mac and the internet to sign again.

## First build for iOS, 2026-11-07

With the iPhoneOS 27.2 SDK, a shard of the translated code compiles with no change, and so do 15 of the runtime's 24 files: the core, the Direct3D queue and Metal
layer's shader translator, threads, libc, Steam, controllers. Nine don't, all for Mac-only APIs: OpenGL in `gl_queue.c`, `pipelines.c` and `shims_gl.c` (Feral's
OpenGL path, which the phone won't have); the Mac's CoreAudio in `shims_audio.c`; Carbon's `FSRef` in `shims_cf.c`; `CGEvent` for keyboard state in `shims_objc.c`;
the ApplicationServices umbrella in `shims_sys.c` and `shims_text.c` (CoreGraphics and CoreText exist on iOS on their own); and AppKit in `mtl_device.m`, for the
window and view the layer draws into.

## Where it stands, 2026-11-07

- `python3 -I tools/build.py --ios` builds `build/ios/libarkham.a` (302 MB): all of the translated code and the runtime, with no errors. The Mac build is unchanged and
  still starts. On the phone `runtime/ios_gl.c` stands in for Feral's OpenGL path; Carbon's file calls are emulated over paths; FMOD gets one stereo 48 kHz stand-in
  device and RemoteIO; the Metal layer draws into the app shell's view, `rt_ios_game_view`; the game's files are looked for in the app's Documents
  (`rt_game_root()`, `ARKHAM_GAME_ROOT`).
- What the game asks of AppKit, from `ARKHAM_TRACE_OBJC=1` on the capsule route (`logs/objc_inventory`, kept local): 182 different messages to system objects and
  36 callbacks into the game's own classes. Foundation exists on iOS. What doesn't, and needs stand-ins of the same names on the phone: `NSApplication`, `NSWindow`
  and `NSView` (the game subclasses both as `FeralWindow` and `FeralContentView`), `NSScreen`, `NSEvent`, `NSMenu`, `NSMenuItem`, `NSCursor`, `NSImage`,
  `NSBitmapImageRep`, `NSGraphicsContext`, `NSOpenGLContext`, `NSOpenGLPixelFormat`, `NSColor`, `NSPasteboard`, `NSWorkspace`, `NSRunningApplication`,
  `NSAppleEventManager`, and the launcher's `WebView`. Most of the window messages set or read sizes and styles; the game starts itself from
  `applicationDidFinishLaunching:` and `runApp:`. The launcher, a web page whose Play button calls into the game's `WebkitJSInterface`, has to be skipped.

Next: the AppKit stand-ins, skipping the launcher, an app shell in `ios/app` that links the library, and copying the game's files onto the phone.

## The app, 2026-11-07

How to put it on a device, with the device connected and unlocked:

1. `ios/app/run.sh [device] install-only` builds the library and the app and installs it, signed by the team in `ios/app/team.local` (or `ios/probe/team.local`).
2. `tools/ios_install.sh [device]` asks this Mac's Steam about the game (`arkham --steam-check`, Steam running and signed in, offline mode fine), and only if
   Steam says the account owns it, copies the game's Steam folder into the app's Documents (9.3 GB: the French, German, Italian, Spanish and Polish packs stay
   behind), Steam's answers to `Documents/Steam/answer.txt`, and the cloud saves to `Documents/Steam/cloud` if the device has no saves yet.
3. `ios/app/run.sh [device]` starts it with its log in `logs/ios/run.txt`; the app also keeps the log in `Documents/Logs/run.txt`. Settings go in
   `Documents/arkham.env`, `NAME=value` lines as in `config.local`; the phone's defaults are Metal, `ARKHAM_LANGUAGE=ru`, `ARKHAM_FXAA=1`.

What makes the phone different, in the runtime:

- AppKit: `runtime/ios_appkit.m` has the 25 classes the game uses, named `ArkhamNSWindow` and so on because iOS has its own `NSColor` and `WebView`; the bridge
  finds them by their AppKit names (`rt_ios_class`). Windows and views keep the frames they're given; the screen is the phone's; menus keep their items;
  `NSApplication`'s run loop turns the thread's run loop. `MainMenu.nib` can't be loaded there, so `load_main_nib` makes its delegate and menu by hand.
- The launcher is skipped (always on iOS, `ARKHAM_SKIP_LAUNCHER=1` on the Mac): its modal loop at `0x2055740` answers as if Play was pressed.
- The game's main thread is not the process's: UIKit keeps that one. `rt_main` runs on a thread with a 256 MB stack, and what the game sends to its main queue,
  its timers and `CFRunLoopGetMain` all go to that thread's run loop (`rt_guest_main_loop`). Other game threads get 32 MB stacks instead of 64 to spare address
  space.
- OpenGL: Feral's code still makes a legacy and a core context at start and asks them about 150 questions. `runtime/ios_gl.c` answers them as the Mac's OpenGL
  did (recorded with `ARKHAM_GL_RECORD=1`), reports one renderer with 2 GB, and makes every other OpenGL call do nothing.
- Displays, the keyboard layout and images moved from `shims_gl.c` to `shims_cg.c`, built for both. The phone's display modes are its screen in pixels and
  75% and 50% of it.
- Steam: `shims_steam.c` answers from `answer.txt` and keeps the cloud saves as files on the device; without the answer, `SteamAPI_Init` fails as it does on a
  Mac without Steam.

First look at the device (the iPhone the user calls iPhone Duo): 51.6 GB free, 3369 MB of memory for the app, and while unfolded a 951 x 669 point screen at 3x,
2853 x 2007 pixels, more than the Mac's 2560 x 1600.

## First play on the phone, 2026-11-07

The game runs on the user's iPhone (iPhone19,4, unfolded 2853 x 2007): menus, a new game, cutscenes and play with a DualSense over Bluetooth, full screen.
What it took after the app shell, all in the iOS paths of the runtime:

- Case in file names: iOS's file system minds it, the game doesn't (it asks for `BMAAData/Binaries` and `BMGame`, which are `binaries` and `BmGame`).
  Paths are looked up again ignoring case when they aren't there (`ci_path` in `shims_libc.c`), also for FSRefs.
- Carbon's HFS paths (`Macintosh HD:Users:...`), through which Feral's code resolves `..`: iOS's CoreFoundation has no HFS style, so `shims_cf.c` converts
  them, with the phone's one volume, `iPhone`, as `/`. FSCatalogInfo got folder numbers and the volume; FSGetVolumeInfo answers.
- The game's main queue on the phone is its own and runs whenever the game pumps events, including Feral's `stop:` then `run` pump; the stand-in event
  pump waits as AppKit's does, so the runtime's lock (not a fair one) isn't kept from the game's other threads.
- Frameworks the phone lacks (`System.framework`, `CoreServices.framework`) get the app's bundle: Feral's code looked them up thousands of times a second.
- Stand-ins return the same objects each time (`[NSScreen screens]` is asked every frame; new ones used up the bridge's 65,536 handles).
- Sound: FMOD and the Bink library's channels are sources of one RemoteIO mixed in `shims_audio.c`; with a RemoteIO each the game went silent after a cutscene.
- The engine's frame smoothing (62 FPS), which a first start writes on, is turned off as the engine reads `BmEngine.ini` (`ARKHAM_FRAME_LIMIT=1` keeps it).
- Tools: `ARKHAM_TRACE_STALL=s` reports every thread when no frame comes for s seconds; `ARKHAM_TRACE_STRINGS=text`; file traces show callers;
  running out of handles reports which code made them. On the phone `ARKHAM_METAL_HUD=1` and `ARKHAM_FPS=1` (a line with FPS, GPU time and the thermal
  state) go in `Documents/arkham.env`.

Measured: settings the game chose on its first start are 1424 x 1000 (half the screen), no MSAA, FXAA on, the highest detail, motion blur on. In play
about 1300 draws a frame, GPU 7.5 ms, game thread 5.2 ms and render thread 4.1 ms of CPU; the menu reaches about 66 FPS, where the game, render and
Direct3D threads and the GPU look to run one after another (about 15 ms in all). The first visit to a place compiles its shaders on the phone: 320 in
14 s after the first cutscene.

Next, in the user's order of interest: shaders built ahead for the phone; the frame's parts overlapping; touch controls like Feral's Tomb Raider for
iOS; a settings screen (resolution, MSAA, 60/120 FPS cap); rumble (the DualSense's haptics engine fails to make a player on the phone); pausing when the
app goes to the background.

## Touch controls, settings and the frame rate, 2026-11-07

- Touch controls (`ios/app/TouchControls.m`), after Feral's Tomb Raider for iOS: a stick where the left thumb lands (past its edge it also runs), swipes on
  the right as the touch pad's right stick (the game ignores the mouse while a pad moves, so mouse look stopped when walking), the main buttons in an arc
  under the right thumb. Three styles: PlayStation, Xbox, or icons of each button's action from the game's own bindings (`BmInput.ini`); the game is shown
  the pad drawn, so its prompts match. In menus, where the game doesn't hold the cursor, a tap is a click: the stand-in `NSApplication` queues events and
  windows hand mouse events to the game's handlers.
- Settings behind a gear at the top right, kept in the app's defaults: frame rate limit (30, 60, 120, none, changed live), button style, size, opacity,
  camera speed, the run push, the frame rate line, the Metal HUD (live), and moving and resizing each button.
- The frame rate stopped at about 65 with every part of the frame mostly idle. `ARKHAM_TRACE_PROFILE=s` showed the game thread waiting 73% of the time in
  `dispatch_sync` to the main queue, which it does several times a frame: the work went into a queue and a bare `CFRunLoopWakeUp`, which doesn't make a
  sleeping run loop run anything, so each hop waited out the loop's timeout. The queue is now a run loop source. The menu went from 65 to 150-210 FPS,
  with up to 117 frames a second on the 120 Hz screen.
- Shaders built ahead: `tools/build_ios_shaders.py` (about 40 minutes for the 32,010 different texts, 16 libraries); `tools/ios_install.sh` copies them.

## Controls v3: outside play, the phone upright, the outer screen, 2026-11-07

- **Outside play** (menus, the map, cutscenes) the pad hides, but for a back button at the top left (the pad's ○ or B, which closes the map, backs
  out of menus and skips cutscenes). The test is the cursor: in play the game warps it back every frame. A game that has stopped warping while its
  frames go on (`rt_frames_presented`, 20 frames and 0.3 s) is in a menu; one that has stopped both is only busy and stays in play. A tap is a click
  at the same share of the game's picture, so it lands right wherever the picture is. After it the cursor goes to the window's bottom right corner,
  where the game draws it off the screen. A drag is the left stick, the other way from the finger, so the map moves with it. The settings can turn
  the hiding off.
- **Taps in the pause menu** did nothing because `[NSApp run]` nested in Feral's event pump returned before handing out the events queued for it.
  It now hands out what's queued, waiting up to 2 ms for more.
- **The camera** is linear: the right stick's push is the swipe's speed, 3000 points a second for the full push at speed 1. The camera then turns by
  how far the thumb moves, however fast. To turn far enough, `BmInput.ini` is read with every right-stick axis 10 times faster and with no dead zone:
  `aTurn`, `aLookup`, and the game's raw `aRawRHJoyRight` and `aRawRHJoyUp`. A real controller's stick gets the dead zone back and a tenth of its push
  in play, so it turns as before. In menus its push is left as it is, since there the game reads it directly as its gamepad mouse
  (`[GFxUI.GamepadMouse]`). The PlayStation pad's 255 stick steps would swallow a slow swipe, so the remainder carries to the next frame.
- **Gadgets**: one button where the cross was. Hold it and the wheel opens; slide to a side and let go to press the pad's cross that way for 0.12 s.
  The game's bindings for those are `bGadgetSelect*`. Let go without sliding and the wheel stays open for a tap on a side.
- **Upright** the game takes the top of the screen in its own proportions (`rt_ios_game_rect`, main.m), and the controls get the space below, with a
  layout of their own. The game's resolution and window don't change.
- **The layout** is kept per orientation (`layout.landscape`, `layout.portrait`). A moved button is a share of the controls' space and its size is
  in proportion to that space's height. The same layout therefore comes out in the same places on the foldable's outer and inner screens, only
  scaled. The first version's layout (offsets from corners) was moved over the first time the phone was held sideways, and was kept as `layout.v2`.
  On another screen the game's picture keeps its proportions instead of stretching.
- **The editor** can hide a button (hidden ones show faintly there), reset one button, and reset all, but only after a confirmation. Layout and
  settings are written through at once.
- **Name and icon**: the app is "Batman Arkham Asylum" on the home screen. Its icon is the Steam version's own (`GameIcon_STM.icns` in the game's
  Mac app): `tools/ios_icon.py` fades its Mac frame to black and writes `build/ios/Assets.xcassets`, and `ios/app/run.sh` runs it before each build.
  Like everything taken from the game, it stays out of the repository.
- **Shaders built ahead**: with the entry renamed in the text instead of by `-D`, every compile has the same options and one module cache, 19 MB.
  Before, it was 483 GB in a day. All 32,010 compiled and the 16 libraries (263 MB) are on the phone. The first launch with them took every shader
  from them (`86 of 86 ... built ahead`), and the title menu ran at about 345 FPS.

## Controls v4: the camera for real, buttons that follow the picture, 2026-11-07

- **Why the camera didn't turn:** the engine reads its input settings with `open()`, not `fopen()`, so the v3 rewrite never ran. The default
  dead zone of 0.2 then swallowed the linear swipes. The rewrite now runs in the `open` shim for any `*Input.ini` (`BaseInput.ini`,
  `DefaultInput.ini`, `BmInput.ini`, `UserInput.ini`). It hands the engine an unlinked temporary file of the same length, since the engine
  may take the size from `stat()`. It sets `Speed=10` (with its sign) and `DeadZone=0` on every right-stick axis, rather than multiplying,
  because the engine writes what it read back into `BmInput.ini`. The camera speed setting moved to a new key (`cameraSpeed`, 1.0) because
  the old one had been pushed to its maximum while nothing turned.
- **Play** is now the cursor held and at least 200 draws a frame (with 0.6 s of grace). Films and loading screens hold the cursor but take a
  few dozen draws, so the pad stays hidden through them and through the start, until play is first seen.
- **Buttons follow the picture.** Every 6th frame `mtl_device.m` copies the finished frame, makes its mip levels and reads back the 44 x 31 level
  (`rt_picture_luma`). Each button's ink glides, over about a quarter of a second, to light lines over a dark picture and dark ones over a light
  picture. Over the black around the picture they're light. Visibility (the "Заметность" setting) now has a floor.
- **Icons:**
  - figure.boxing for the strike;
  - bolt.shield for the counter;
  - tornado for the cape;
  - figure.climbing for the grapple;
  - camera centring for L3;
  - a drawn batarang for the gadget wheel and for R2 while L2 aims, R2 then lit, since that's where the game throws.
- **Photo mode:** an eye button (top left; upright, between map and pause) hides every button, the gear and the frame-rate line. The stick and
  the camera keep working unseen, and a tap brings everything back.
- **Menu clicks:** the cursor moves first, the button goes down 50 ms later and up at 120 ms. The cursor goes to the corner 1.2 s after the
  last tap. A click sent together with the move came every other time. For finding where the game draws its cursor, each tap saves two frames
  to `Documents/Shots` (just after the click, and after parking). The copy is written off the game's threads, so nothing waits on it.
- **Settings** are a grouped table in a sheet, each control under its name and as wide as the table. The Metal HUD follows its switch only:
  `MTL_HUD_ENABLED` is no longer set from `arkham.env`.
- **Not shaders:** the lags at level loads, turns and fights come from pipelines. In the first long session, all 591 shaders came from the
  prebuilt libraries (0 ms compiling), but 1,280 pipeline states took 4.7 s to create: the driver's final compile for this GPU, on first use. They
  came in bursts of 30 to 100, as frames of 100 to 380 ms.

## Controls v5: the camera through the mouse's axes, pipelines off the game's thread, 2026-11-07

- **Why swipes felt like a joystick.** The game takes the right stick through its own joystick handling, with acceleration and the camera's own
  ideas while walking. The mouse turns the camera directly, but the game drops the mouse while a pad's stick is held, and walking holds one.
  Scripted runs on the Mac (`tools/walk.sh` with the new `pad`, `shot` and `look` steps, and `ARKHAM_FAKE_PAD`) showed it:
  - standing, 20 mouse moves of 40 points turned the camera about 180 degrees;
  - the same moves while the pad walked turned nothing;
  - the pad's right stick bound to the mouse's axis (`aMouseX`) turned the camera while walking too.
- **How the camera works now.** On the phone, `shims_libc.c` rewrites every `*Input.ini` as the engine opens it:
  - the right stick's `aTurn`/`aLookup` become `aMouseX`/`aMouseY` at Speed 600, with no dead zone;
  - the stick's raw axes get Speed 5;
  - `bEnableMouseSmoothing` is off. It had taken most of a stick's turning, which counts no mouse samples.
- **Measured on the Mac.** The mouse axis is applied once a frame: about 0.0305 degrees per unit, so 18.3 degrees a frame at the full push.
  The same push turned 2.5 times less at 60 FPS than at 155.
- **What a swipe sends.** `TouchControls.m` sends the thumb's speed divided by the game's measured frame rate. The camera then turns 0.284 degrees
  per point at speed 1 (about 270 degrees across the screen's width), at any speed or frame rate.
- **Directions.** A thumb going up looks up; the game's own default is inverted (stick up looks down), and a setting inverts it back.
- **A real controller** on the phone gets a 0.15 dead zone and a fifth of its push. That turns about 220 degrees a second at 60 FPS, and the raw
  axes (Speed 5) still see all of it.
- **Careful:** the engine writes the input settings it read back into `MacInit/BmInput.ini`. A run on the Mac with `ARKHAM_RIGHT_STICK_MOUSE`
  leaves the Mac's game with the stick on the mouse, so the experiment's lines were restored by hand afterwards.
- **Pipelines** (the lags): on the phone, a pipeline the system hasn't compiled before is built off the D3D thread. Its draw waits at most 2 ms,
  and at most 4 ms in all a frame, then is left out until the pipeline is ready. The system cache makes revisits fast: the second session's
  1,028 pipelines took 212 ms, against 4.7 s for 1,280 the first time. A setting turns this off, and so does `ARKHAM_SYNC_PIPELINES=1`.
- **Startup:** `Documents/Logs/run.txt` lines now start with seconds since the process started (from the kernel), for finding the 15 s of black.

## Controls v6: the camera from a frame's share through the game's measured mouse curve, 2026-11-07

- **v5's swipes still felt like a joystick** for small moves. The PlayStation pad has 255 stick steps, and the runtime spread the remainder over
  display frames (1, 0, 1, 0 at 120 Hz). The game reads 60 times a second, so it saw only the zeros or only the ones.
- **The camera hook.** The game warps the cursor once a frame in play, right after reading its input. At that moment the runtime now hands over
  exactly the thumb's turning since the last frame (`rt_touch_camera_frame`, `shims_hid.m`). It pushes the value at the game loop's next pass,
  before the next frame reads it, through a run loop source, not from inside the warp.
- **Measured with scripted Mac runs.** `walk.sh` steps `pad`, `shot`, `tlook`; shifts of the picture are measured from frames.
  - The pad code drops a push under about 0.05 (6 of 127.5 steps).
  - With the stick on `aMouseX` at Speed S and mouse smoothing off, the game turns the camera by about `0.038 · x^1.9` points of a 712-point
    picture a frame, where x = S·push. That is the mouse's acceleration, about 7 points to a degree.
  - The curve fits every hold: x from 1.4 to 19, at 60 and 155 FPS, and the same per frame at both.
- **The runtime inverts that curve** at Speed 60. The smallest turn the pad lets through is about 0.05° a frame, and the largest about 12°.
  The game smooths the stick over a few frames, so a lone one-frame push comes out far smaller. A finger's continuous travel is what the
  inversion is for.
- **Checked on the Mac at 60 FPS** through the same path as a finger, at 60 shares a second: 10° came out as 69 points (9.9°), 12° fast as
  about 11°, and there and back left about 3°.
- **A real controller** gets 0.055 to 0.55 of a push: the smallest that passes, up to about 250°/s at 60 FPS. The raw axes read at Speed 1.8,
  so they still see the full push.
- **Sound gaps** came with bursts of hundreds of pipelines building at once. The game's sound threads now run at user-interactive priority, and
  pipelines build three at a time at utility priority.
- **Mac safety.** A Mac run with `ARKHAM_RIGHT_STICK_MOUSE` sends the engine's writes of `*Input.ini` to `/dev/null`. One write still got
  through and regenerated `MacInit/BmInput.ini` with the game's defaults, which is what it should hold.
