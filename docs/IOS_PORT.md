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
- **AltStore takes Batman's profile away (2026-11-09).** AltStore is on the phone under the same Apple account, and AltServer runs on the Mac. Each time AltStore
  refreshes or installs through AltServer, it removes every free provisioning profile on the phone that isn't one of AltStore's own apps (AltServer's
  ALTDeviceManager.mm, removeAllFreeProfilesExcludingBundleIdentifiers). That includes Batman's and the probe's profiles. The phone then says Batman is "no longer
  available" and its profile is unavailable, though the profile's dates and the certificates are fine (Apple's OCSP said "good").
  - The fix needs no rebuild. `xcrun devicectl device install app --device <UDID> $TMPDIR/arkham-app-build/Build/Products/Release-iphoneos/Arkham.app` puts the
    same build back with its profile. The app's data stays: the game's files, saves and the Steam answer.
  - Deleting the app instead would lose all of that.

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

## Controls v7: the camera is the game's mouse again; startup's path lookups remembered, 2026-11-07

- **The user's verdict on v4 to v6:** the camera felt like a joystick whatever was done to the stick, and the first version, which moved the
  mouse, had felt right except that walking stopped it. So the stick camera is gone (`rt_touch_look`, the frame hook in the cursor warp, the
  curve inversion, `ARKHAM_RIGHT_STICK_MOUSE`, `ARKHAM_TEST_TOUCH_CAMERA` and the `tlook` step), and a swipe is the pointer's motion again.
- **Why walking stopped the mouse:** Feral's input loop (F_19deea0) hands the pointer's motion to the engine as the MouseX and MouseY axes
  through the viewport client's InputAxis (vtable slot 0x10, the engine's at 0x00dee280), marked as not from a pad, and the engine ignores
  those while a pad is in use. The touch controls are a pad, so a swipe counted for nothing while the left stick walked.
- **The fix, in the game's own path:** a host function stands in front of that InputAxis in the dispatcher (`input_axis_as_pad`,
  `shims_sys.c`) and marks only those two calls, known by their return addresses 0x19df2ea and 0x19df353, as from a pad. The camera then
  turns through the game's own mouse bindings, walking or not. It is on on the phone (`ARKHAM_MOUSE_NOT_PAD=1` turns it off) and on the Mac
  with `ARKHAM_MOUSE_AS_PAD=1`.
- **Whole pixels:** the game turns the pointer's offset from the centre into whole pixels and warps back, so a fraction would be lost each
  frame. On the phone the runtime hands over whole pixels only and keeps the remainder for the next frame (`rt_touch_mouse`).
- **The phone's input settings** as the engine reads them (`shims_libc.c`, same length, on disk unchanged):
  - `MouseSensitivity` 7.5 instead of 30, so a pixel is a quarter of the turn and slow swipes turn smoothly;
  - mouse smoothing off: with one sample a frame, at 60 FPS UE3's smoothing doubles the motion of frames a little under 1/60 s and not
    of the others, so the same swipe came out different;
  - the right stick's lines back to the game's own (aTurn 1.0, aLookup 0.8 or 1.0 with the raw axis, dead zone 0.2, raw axes 1.0), where v5
    and v6 had put them on the mouse's axes and the engine had written that into the phone's `BmInput.ini`. A controller is as in the original.
  - `ARKHAM_TOUCH_MOUSE_INI=1` gives the Mac the same, with the engine's writes of those files sent to `/dev/null`.
- **Measured on the Mac** with the phone's settings at 60 FPS (frames' shifts in points of a 712-point picture): 96 pixels gave 27, 28 and
  27 points sent as 24 steps of 4, 4 of 24 and one of 96, and 288 back gave -81. So the turn is linear and doesn't depend on speed:
  7.5 of the engine's rotation units a pixel (65536 to a turn), 0.041°.
- **The swipe:** 0.284° a point at speed 1, about 270° across the screen, which is 6.9 pixels a point. The speed setting has a new key
  (`mouseCameraSpeed`, 1.0), because v6's camera had been set to its maximum of 3.0, which on this camera would be three times too fast.
- **Startup, 18 s on the phone against about 4.3 s on the Mac.** The phone's log had nothing between 2.9 s and 15.6 s. On the Mac, the
  startup makes about 75,000 `stat` calls, 40,800 of them for files that aren't there in any case: the engine looks for each localization file
  in four folders (BmGame's and Engine's, RUS and INT), BmGame.RUS some 5,280 times in each. On the phone, whose disk doesn't ignore case,
  each of those was looked for again one name at a time: an `lstat` for every folder in the path, then a scan of the last folder's names.
- **Now each lookup is remembered** for the path as asked, with the folder the answer came from, and used again while that folder's
  modification time stays the same. A new file or folder in it changes that time. A test with a case-sensitive disk image covered folders
  and files in another case, missing files and a file added later. On the Mac, 41,000 lookups of missing localization files took 1.85 s
  looked up afresh and 0.26 s remembered, about 7 times less. Phone syscalls cost more, so the gain there should be larger.
- **The phone's log** now has a `[ci]` line every 10 s while lookups happen: how many paths weren't there as asked, how many were answered
  from memory, how many were found in another case, and the time spent. On the Mac, `ARKHAM_CI_PATHS=1` turns the lookup on, and
  `ARKHAM_CI_NO_MEMO=1` gives the old way.
- **The slow seconds after the first cutscene** in the v6 log: about 8 s at 15 FPS with frames 75 ms apart. The engine's game thread
  (1dab9d0) took about 67 ms of CPU a frame, while the render thread and the GPU were mostly idle. New packages coming in look for their
  localization the same way, so this is likely the same cost. Not confirmed yet: the next phone log shows it.

## Controls v8: the touch camera straight to the engine; opacity and the Metal HUD switch fixed, 2026-11-07

- **v7 on the phone:** standing still, the camera felt right; walking, it hardly turned. The cutscene-to-play lag was gone, and the first picture
  came at 8.2 s instead of 18.7 s (sound at 5.5 s instead of 15.6 s).
- **Why walking still dropped it:** Feral's input loop reads the pointer, then handles its queue of events, and only then checks the counter
  at window+0x4d4: while it's above zero the motion is thrown away and the counter goes down by one. A pad's stick events raise it. The scripted
  Mac test held the stick still, so there were no events; a thumb on the touch stick is never quite still. The v7 hook fixed only the engine's
  half (the calls marked as not from a pad).
- **Now:** at the loop's reading of the pointer (HIGetMousePosition called from F_0048c10 from F_19deea0), the runtime calls the engine's
  InputAxis itself (`touch_mouse_to_engine`, `shims_sys.c`) with what the loop would pass, and marks the calls as from a pad:
  - the loop's viewport client `[ebp-0xc0]`;
  - its window `[ebp-0xc8]`, with the viewport 4 bytes in;
  - the frame's time `[ebp+0xc]`;
  - the names MouseX and MouseY from its globals (`[ebp-0xbc]` = 0x19deeb1, +0x1aaf027 and +0x1aaf02b).

  The pointer stays where the game put it, so the loop sees no motion and its counter doesn't matter. Fractions of a pixel go through as they are.
  Motion the game hasn't taken for 250 ms is dropped. The log has a `[mouse] touch camera` line every 10 s. `ARKHAM_MOUSE_AS_PAD` is now only for
  the Mac's own mouse.
- **"Заметность"** set only 70% to 100% of the icons' alpha, so the slider seemed to do nothing. It is now the whole button's opacity, all of it
  while pressed, the stick's too, with a default of 80%. At the user's wish it's called "Прозрачность кнопок" and shown as transparency
  (0 to 80%, more to the right); the setting stored is still the opacity.
- **The Metal HUD switch** had no effect: `developerHUDProperties` only shows and hides the HUD in a process it was enabled for from the launch.
  The switch's state now decides that at launch (`MTL_HUD_ENABLED` and Metal's `MetalForceHudEnabled` default). Then the switch shows and hides
  it live. Turned on in a game started without it, it says the HUD comes at the next launch.
- **Path lookups** remembered in v7 were each checked with a `stat` of their folder, about 0.8 s of the startup on the phone. Now a check happens only
  after the game made, renamed or removed a name through the shims (open with O_CREAT, fopen for writing, mkdir, rmdir, unlink, remove, rename),
  or after 2 s, for files made some other way. The case-sensitive image test covers both.
- **What's left of the black screen** (v7 log): engine start to sound 4.2 s (Mac 2.6 s); the back buffer to the first frame 2.6 s (Mac 0.3 s);
  then about 3.8 s of black frames before the logo film, the same on the Mac. `ARKHAM_TRACE_PROFILE=0.5 ARKHAM_TRACE_PROFILE_FOR=13` samples
  every thread for only the first 13 s.

## Controls v9: the vertical, the menu cursor, camera assist; where the rest of the startup goes, 2026-11-07

- **v8 on the phone:** up and down came out inverted; the save menu showed the mouse cursor; running, the camera didn't turn at all, though the
  log showed the swipes reaching the engine (623 frames with motion).
- **The vertical:** F_0048c10 gives MouseY as the centre minus the pointer, so up is positive; v8 passed the pointer's own direction. Fixed in
  `touch_mouse_to_engine`.
- **The cursor:** v7's hook, which marks the input loop's own MouseX/MouseY calls as from a pad, had kept the menus with the pad, so they never
  showed their cursor after a tap moved the pointer. v8 had turned it off on the phone; it's on again (`ARKHAM_MOUSE_NOT_PAD=1` turns it off).
  The pointer still moves and clicks where the finger taps; it just isn't drawn. The user wants exactly that.
- **Running:** the game's camera assist (`EnableCameraAssist` in `[BmGame.R3rdPersonCamera]`, BmCamera.ini, True by default) turns the camera
  behind Batman as he runs when a pad is in use. With the touch camera reaching the engine as a pad, it won over the swipe. The camera settings
  are now rewritten on read like the input ones. A new switch, "Помощь камеры" (off by default, from the next launch), sets it through
  `ARKHAM_CAMERA_ASSIST`. Not confirmed yet that this was all of it.
- **"Заметность"** is now "Прозрачность кнопок".
- **The startup** in this version: sound at 4.7 s, the first frame at 7.3 s, the logo film at 11.2 s (v6: 15.6, 18.7 and about 22 s). Path
  lookups now take 29 ms in all.
  - A startup profile (`ARKHAM_TRACE_PROFILE=0.5 ARKHAM_TRACE_PROFILE_FOR=14`, through `devicectl ... launch -e`) put the game thread from
    5.4 s to 7.2 s almost entirely in throwing: Unreal throws a string from StaticLoadObject for each object it doesn't find and catches it
    itself. The runtime then looks for the handler's native frame with libunwind, and the phone spent its time inside libunwind.
  - The first throw took 8 ms for 4 steps. `[eh]` lines give the running totals (throws, time, steps) at a throw at most every 5 s.

## Controls v11: the touch camera through the right stick's axes, 2026-11-08

- **v10 on the phone:** menu taps worked again (the cursor visible). With the DualSense, in the same place, the camera turned normally while
  Batman walked; the swipe hardly did.
- **What the Mac runs showed:** every swipe reached the engine while walking (`ARKHAM_TRACE_TOUCH_MOUSE`, 24 hand-overs out of 24). With the
  stick held a little and wandering but Batman standing, a swipe turned as far as standing (28 points against 29). So the game damps the
  mouse's turning only while Batman moves, and lets the right stick's through. In v5 and v6 the right stick fed the mouse's axis and was bad
  while walking too, so it's the axis, not the device.
- **Now** the mouse's bindings (`MouseX`, `MouseY`, BaseInput.ini and BmInput.ini) are read as `Axis aTurn Speed=1` and `Axis aLookup Speed=1`,
  the right stick's own axes, with no dead zone. The runtime still calls the engine's InputAxis directly with MouseX/MouseY, from Feral's
  input loop, but with the value a stick would need this frame to turn by exactly the swipe:
  - across: pixels x 7.5 / (100 x frame time x 375);
  - up and down: the same with -300, LookRightScale and LookUpScale.
  This isn't the stick emulation of v4 to v6: there are no 255 steps, no curve or dead zone of Feral's pad code, and no tick between frames.
  It's one exact value a frame. `ARKHAM_TOUCH_TURN=0` gives v10's mouse axes back.
- **Not measured:** Steam wasn't running on the Mac, and the Mac game stops in the Steam API at startup without it, so this went to the phone
  with the arithmetic from UE3's PlayerInput. If up and down come out inverted, `TOUCH_TURN_VSIGN` in shims_sys.c is the sign to flip.
- **Mac test tools added for this:**
  - `ARKHAM_MOUSE_DIRECT=1`: scripted `look` steps take the touch camera's path;
  - `jit:n`: the fake pad's held sticks wander like a thumb;
  - `mpeek`: shows the values a memory scan kept;
  - `ARKHAM_TOUCH_TURN=1` with `ARKHAM_TOUCH_MOUSE_INI=1`: the phone's bindings.

## Controls v12 to v15: walking by the keyboard's names, the map, the hidden cursor; new icons, 2026-11-08

- **v13, the frame rate:** the drop to 37 FPS that v12 showed came from Feral's own `Preferences Data`: after a launch on the folded phone's outer screen it fell back to
  the full pixels, 2848 x 2000 on the inner screen. The phone now reads ScreenW/ScreenH there as half the screen's pixels (1424 x 1000),
  and half is the default display mode.
- **Why the swipe camera hardly turned while walking.** Measured on the Mac with the same route twice and the phone's settings
  (`ARKHAM_TOUCH_MOUSE_INI=1`, so both runs had the same sensitivity): a 400-pixel turn while walking with the pad's left stick came
  out at about a fifth (21 points against 117), while walking with the W key it came out whole. The game damps the camera's turning while
  Batman walks by the pad's left stick keys (`XboxTypeS_LeftX/LeftY`), from any source; marking those events as not a pad's, or sending
  them past the pad, didn't help. Sensitivity wasn't the cause.
- **v14, the fix:** the touch stick's position goes to the engine each frame as `StrafeRight`/`MoveForward` (`aStrafe`/`aBaseY`, analog),
  the keyboard's movement names, and as `TurnRight`/`TurnLeft` rebound on the phone to the raw left stick axes (`aRawLHJoyRight`,
  `aRawLHJoyUp`) that the keyboard also feeds. The pad the game sees keeps its stick in the middle in play. The names are found in the
  engine's name table at startup. The user's verdict: the camera behaves while standing, walking and running.
- **v15 (built, not on the phone yet), the map:** walking goes straight to the engine only while the game warps the cursor each frame, which it does only in play. The
  map and menus get the pad's stick again, so the map pans.
- **v15, the menus' cursor:** the interface draws it into the frame itself, as the frame's last two draws (the arrow's fill, 12 vertices and
  10 triangles, and its outline, 16 and 24, 8-byte vertices, the 1024 x 1024 L8 shape cache as texture 0). Found on the Mac with
  `ARKHAM_FIND_CURSOR`. On the phone those draws are left out when the frame before ended with them; the pointer still moves and clicks
  where a tap puts it. `ARKHAM_SHOW_CURSOR=1` draws it on the phone, `ARKHAM_HIDE_CURSOR=1` hides it on the Mac.
- **v15, the Metal HUD switch** stores the choice for the next launch and says so each time: iOS shows Apple's HUD only in a process it was on
  for from its start.
- **v15, icons:** `ios/app/BatIcons.h` draws each button's icon with Core Graphics in the batarang's manner, in place of Apple's symbols:
  chevrons to run, a fist, the counter's lightning, the cape, a grappling hook, down to the ground to crouch, a magnifier, the cowl for
  detective mode, a reticle, an arrow round the cowl for the camera, the map, pause, a struck-out eye, a cross. `tools/bat_icons_preview.c`
  draws them all on the Mac as the app does.
- **v15, colour:** the buttons are white on their dark discs by default (Settings, "Кнопки", "Цвет": "Белые"); following the picture, light over
  dark and dark over light, is the other choice. While white, the picture's lightness isn't read back.
- **The startup on the phone (v14 log):** the launcher at 1.0 s, sound at 4.6 s, the back buffer at 4.7 s, the first frame at 7.3 s, the
  Batman logo film at 9.0 s.
  - The 2.5 s between the Steam calls after the device is made and the first frame take 0.1 s on the Mac. Only one exception is thrown
    at startup, 8 ms, so it isn't throwing. The first launch of each new build now profiles every thread for its first 10 s, so the next
    launch's log shows where they go.
  - The 3.8 s of one-draw frames after the device is made, which looked black in the logs, are Feral's own logo film (`feral.bik`,
    96 frames at 25 a second, no sound). On the Mac it shows as it should.
- **Still open:**
  - the gadgets' button showing the gadget chosen (the game's choice isn't known outside it yet);
  - DualSense rumble on the phone;
  - the cryptographic sequencer with touch;
  - resolution and MSAA as settings.

## v16: even frames, readable badges, no startup films, 2026-11-08

- **The first launch's profile (v15):** the startup's open 2.5 s, between the Steam calls after the device is made (5.2 s) and the game
  setting its cursor association (7.7 s), are the game thread in `runtime/eh.c`. It is in phase 2 of the one exception the game throws
  at startup and catches itself; phase 1 and the native frame took 8 ms. The Mac goes through the same in about 0.1 s. Not found yet:
  where in phase 2 the time goes (the profiler names static functions by the nearest symbol, so `phase2 < lsda_lookup` may be either).
- **Frame pacing:** with the user's limit of 60, the game made an even 60 frames a second, but the phone showed them unevenly: in 5.5 min
  of play a third of the frames stayed 8.3, 12.5, 20.8 or 25 ms instead of 16.7. The limit only slept to the next 1/60 s; frames then
  reached the ProMotion display a little early or late, and a late one was followed by a short one. Now, with a limit, each frame is
  presented with `presentDrawable:afterMinimumDuration:` at the limit's step (`present_paced`, `ARKHAM_MTL_PACE=0` for the old way). On the
  Mac a limit now also turns the layer's display sync on, since a frame can only be held a step on a synced layer. The `[fps]` line
  counts the frames that stayed off the step by more than 2 ms.
  - Mac A/B, the same scripted run into the level, running forward while turning, limit 60: off the step 115 of 1560 frames (7.4%,
    up to 20 in a second, gaps up to 25 ms) the old way, 0 the new way (longest gap 16.7 ms). The cost: from present to screen 37 ms
    instead of 20, one frame waiting in the display's queue, which is what absorbs a late frame.
  - Running on the phone the game makes 53 to 55 frames a second with the GPU busy 7 to 8 ms: that is the game thread, and evenness
    can't fix it.
- **Badges:** the PlayStation symbol in a button's corner was drawn by the button's own symbol layer, under the badge's 75% dark disc,
  so only a trace of it showed, at the button's opacity besides. The badge (disc, symbol, name) is now a layer of its own over the button:
  a disc with a light rim, a larger symbol with a thicker line, and at least 90% opacity whatever the button's transparency.
- **A leftover removed:** each tap in the menus saved two screenshots (Documents/Shots), from finding where the game draws its cursor.
- **No startup films:** the engine reads `[FullScreenMovie] StartupMovies` (feral, baa_logo_run_v5_h264, UTlogo, Legal, Install) from
  BmEngine.ini; with none listed it has no film to wait for and shows its title screen as soon as that is loaded. On the phone they are
  left out of the text it reads unless `ARKHAM_INTRO=1`; on the Mac only with `ARKHAM_INTRO=0`, since scripted runs click through them.
  The engine writes BmEngine.ini back at each start from what it read, so the phone's copy loses those lines for good (bringing the
  films back would need them written in again). The Mac check kept MacInit's copy and put it back after the run.
  - Mac, no films: the engine starts at 7.2 s (after the launcher's scripted Play at 6 s), the first frame is at 11.7 s, and that is
    already the title screen, Batman on the tower and "Щелкните, чтобы начать", with no black wait.

## v17: frames after turning the phone, the map by touch, readable buttons, 2026-11-08

- **Uneven frames came back after turning the phone.** In the v16 log the limit of 60 held every frame for 16.7 ms, and none were off the step
  for a minute (present to screen 25.5 ms, one frame waiting in the display's queue). Then the phone was turned upright and back (106 to 110 s).
  From then to the end, about 40 frames a second were off the step again (25 and 8.3 ms apart), with present to screen 21 ms: iOS no longer held
  the layer's frames for the minimum duration, though it kept its place and size. The user had put it down to switching the limit to 120 and back;
  the log says the turn. Now the app makes a new layer for the game once the view has settled at a new size (`rt_mtl_new_layer`): the new one goes
  under the old, which leaves 150 ms later. Not yet seen on the phone.
- **Tried and dropped: keeping the processor fast at the limit.** Spending the last 4 ms of the limit's wait busy rather than asleep (so that
  iOS wouldn't lower the clock) changed nothing on the Mac. The game's busiest thread uses about 95% of each frame whatever the limit, 5.9 ms
  at 159 FPS and 16.3 ms at 60, waiting busily, so its CPU time a frame says nothing about the clock. v16 held 60 while running on the phone;
  the 53 to 55 of the user's v15 screenshot doesn't recur in the v16 log.
- **Badges** in the buttons' corners are at the button's opacity again (v16 kept them at least 90% opaque; the user wants them to fade with it).
- **Buttons over a light picture:** the picture-following colour is gone (the user found it unreadable on white). Each icon has a soft dark ring
  under it (`ios/app/BatHalo.h`: the icon drawn round two circles in its own layers, made black, the icon itself knocked out), and the disc a
  shadow round it.
- **The map** (found on the Mac, from a read-only copy of a later save given as slot 3 through `ARKHAM_FROZEN_DIR`): its hints say Select closes
  it, R2 and L2 zoom in and out, square is "Show Batman", and L1 and R1 change tab. The left stick moves the map's view, so the map goes the other
  way. At full tilt that is about a third of the picture's width a second at the first zoom, and twice that zoomed in. Half tilt is about two thirds
  as fast. L2 zooms out by 0.62 in a quarter of a second. Show Batman zooms all the way in.
  - On the phone, after the map button the controls know the map is open. A drag adds the finger's travel to what the map has still to do, and the
    stick is tilted to do that within about a tenth of a second, at least 0.3, so that a swipe ends where the finger did. Two fingers zoom, a
    trigger held while there's zoom left to do. A "Показать Бэтмена" button (square) shows at the bottom right.
  - The map is over when play comes back, or with the back button. If the map button brought no map within 1.5 s, it isn't taken as open.
- **The startup's 2.5 s**, with more to go on at the next first launch.
  - `[eh] a throw of ... reaches its handler` gives the time from the throw to its handler, the cleanups on the way, and the walk over running
    handlers (`run_top`), the one loop in phase2 that calls nothing.
  - The profiler now gives the innermost frame in the runtime's own code as function+offset.
  - On the Mac the throw reaches its handler one frame up, 7.7 ms after it was thrown, with 0 steps.
- **The first launch of a build** also profiles from 90 to 110 s, once every 2 s (`ARKHAM_TRACE_PROFILE_PLAY=90:20:2`). The Mac's profile shows
  "?" for the busiest threads, so the phone's is the one that names the translated functions to make faster.
- **Icons:** the counter is now a shield with the blow sent back (`bi_shield`), where the lightning didn't read as counter. The fist is drawn
  again from the front, with its finger segments, the thumb across them and a cuff. The map's "Показать Бэтмена" is the bat in a target's ring
  (`bi_findbat`). `tools/bat_icons_preview.c` draws them as the app now does: the halo, the disc's shadow, the badge, over dark, light and grey.

## v18: occlusion queries answered without waiting, all four ways up, 2026-11-08

- **The startup's 2.5 s, narrowed down.** v17's first launch: the startup's one throw reaches its handler one frame up 8.3 ms after it was thrown,
  with no cleanups and no steps over running handlers. So the 2.5 s come after the handler is found. The profile still shows them as
  `phase2 < lsda_lookup < shim___cxa_throw` with no offset on `phase2`, so that innermost frame isn't in the game's executable at all.
  The profiler now names the image of an innermost frame outside it, for the next first launch.
- **What the render thread does in play** (v17's profile, 90 to 110 s, on the phone): the busiest thread is the game's render thread, and 90% of
  its samples are `swtch_pri < run_sync`, yielding while it waits for the Direct3D thread to reach one of its calls in the queue. The Direct3D
  thread itself is mostly idle. Nearly all of those calls are `Query.GetData`, the engine polling its occlusion queries: on the Mac, 45 000 to
  53 000 synchronous calls a second (about 250 a frame), the render thread waiting 30 to 34 ms a second there. On the phone, waking the other
  thread for each costs far more.
  - Now the render thread answers `GetData` itself (`rt_mtl_query_answer`, mtl_device.m) from what the Direct3D thread has already done for the
    query. Each `Query.Issue` is counted when the render thread queues it and when the layer has done it.
    - All of them done, and the GPU past the query's command buffer: the visibility count straight from the shared buffer.
    - Otherwise: not ready, as Direct3D 9 may answer. With the engine's flush flag, a `GetData` with nowhere to write is queued once for each
      Issue, so that the layer still commits the commands the query needs.
  - `ARKHAM_QUERY_SYNC=1` waits the old way.
- **All four ways up:** the foldable's inner screen turned upright one way is upside-down portrait to iOS. The app didn't allow that, so the game
  stayed on its side; turned the other way, it turned (v16's log has it upright on the inner screen). The Metal HUD starting over with only
  its FPS line, after a fold or a turn, is the new layer that v17 makes then: the HUD belongs to the layer.
- **Mac A/B of the queries,** the same walk into the level, no limit:
  - synchronous calls 45 000 to 53 000 a second down to 330 to 440 (`Texture.LockRect` and resource creation are what's left);
  - the render thread's waiting 30 to 34 ms a second down to 7 to 8;
  - 183 against 181 FPS, the Mac's render thread not being what holds it;
  - shots at the same moments show the same scene, nothing missing.
  On the phone that wait was most of the render thread's time; what it's worth there, the next play profile will say.
- **A new layer for a new limit too:** in v17's log, after 120 and back to 60, iOS held the frames unevenly for some 15 s before it settled,
  as after a turn. `rt_set_fps_cap` now makes a new layer when the limit changes.

## v19: back to waiting for the queries, the startup's 2.5 s in libunwind, which way the phone is held, 2026-11-08

- **The query change undone (opt-in now, `ARKHAM_QUERY_FAST=1`).** With it on the phone (v18), at the limit of 60 the game made 60 frames a
  second but only 53 to 58 reached the screen, with gaps of 25 to 33 ms; in v17 all 60 did. The render thread no longer waited, ran ahead in
  bursts, and turning the camera felt uneven to the user, who found v17 smoother. The GPU's time a frame was also higher, more of the scene being
  drawn while its queries weren't answered yet.
- **The startup's 2.5 s are in libunwind.** v18's profile, with the image of the innermost frame, shows
  `<redacted>(libunwind.dylib) < <redacted> < <redacted> < phase2`. The earlier reading of "phase2" as the innermost frame was a mistake in the
  reading: the `<redacted>` frames had been cut out of the lines before looking at them. After the handler is found, phase2 calls libunwind only
  in `unw_resume`, once the function holding the handler has returned. `[eh] the handler's function returned ... ms after the throw` now says
  when that is; on the Mac it is 8.3 ms, and the Mac's whole gap is 0.1 s.
- **Which way the phone is held:** each turn now logs the phone's orientation and the app's, to tell whether iOS refused a turn of the inner
  screen or never saw one. v18's log has the inner screen upright once (669 x 951 points), yet the user found it didn't turn.
- The first launch of a build no longer profiles 90 to 110 s in play: the sampling itself made play stutter (`ARKHAM_TRACE_PROFILE_PLAY` on asking).

## v20: frames on the display's beat, one layer, the startup throw resumed by the runtime, 2026-11-08

- **The startup:** the user's screen recording shows about 8.5 s of black from the tap to the title. v19's log: the handler's function returned
  8 ms after the startup throw and the game went on 2.6 s later, all of it in libunwind's `unw_resume`. `resume_at` (eh.c) now does what it
  does on arm64 itself: the frame's x19 to x28, frame pointer, stack and program counter and d8 to d15 from the cursor, then a jump there.
  `ARKHAM_EH_UNW_RESUME=1` uses libunwind's. On the Mac the game starts and plays with it.
- **Frames at a limit on the phone:** v19 at 60 still had fewer than 59 frames on screen in 42 seconds of 135. The limit slept to a timer of its
  own, which drifts against the display. Now a display link at the limit's rate, on a thread of its own, gives the beat that each frame waits for
  (`beat_wait`), and asks the display for that rate (`ARKHAM_MTL_TIMER_LIMIT=1` for the timer).
- **Why turning the phone broke the pacing (v16):** the touch controls' own display link asked for 120 whatever the limit. After a turn the display
  ran at 120 (present to screen fell from 25.5 to 21 ms), and a game at 60 landed on 8.3 ms steps as it fell. The controls' link now asks for no
  more than the limit. The new layer made after each turn or fold (v17 to v19) froze the picture for a moment and is gone: one layer again.
- **The inner screen upright:** v19's log has the phone held on its left side turning the app upright, and on its right side not. iOS refuses
  upside-down portrait on iPhone whatever the app allows, and for the inner screen that is one of the two upright ways.
- `docs/WISHLIST.md` keeps the owner's open wishes and what has been done, so that none is lost between rounds.
- **v20 on the phone** (the user's log, 337 s):
  - The startup throw's handler is left 40 ms after the throw, against 2.5 s: the first frame came at 6.05 s instead of 8.28 s.
  - At the limit of 60 for 267 s: 59.9 frames made and 59.7 shown a second. One went missing in 15 seconds, against 42 of 135 in v19.
    Off the step 0.4 a second, gaps 16.7 ms.
  - Folding and unfolding (149 to 192 s) left it even.
  - The user: "по FPS всё стало намного лучше и стабильнее", and folding is fine.
  - With Low Power Mode on: about 50 made and 46 shown.

## v21: MetalFX, and the game's resolution as a setting, 2026-11-08

- **MetalFX's spatial upscaler between the frame and the screen.** The game's frame, drawn at a share of the screen's pixels, used to reach the
  screen at its own size, stretched by the compositor. Now it is scaled up to all of the screen's pixels by `MTLFXSpatialScaler` (perceptual
  colour, the frame as anti-aliased input after FXAA) on the present queue, straight into the drawable when that can take MetalFX's writes.
  The drawable is then the screen's size.
  - The app tells the runtime the picture's size in pixels whenever it moves (`rt_mtl_set_screen_pixels`).
  - Nothing is scaled when the screen's pixels aren't larger than the frame, as on the outer screen.
  - Settings, "Изображение": "MetalFX", on by default, at once. `ARKHAM_METALFX` sets it at the start. On the Mac, `ARKHAM_METALFX=1` and
    `ARKHAM_METALFX_SCALE` (1.5) try it.
- **Mac check** (2560 x 1600 scaled to 3840 x 2400): the game runs at 172 FPS with 120 a second on screen, as without it. The picture is right.
  Apple's Metal HUD shows a MetalFX block: input 2560 x 1600, target 3840 x 2400, spatial.
- **"Разрешение игры"** (high, medium, low): 0.5, 0.42 or 0.375 of the screen's pixels, taken at the next start. `ARKHAM_RENDER_SCALE` is
  read by the preferences rewrite and by the display modes, which put that share first as the default. Feral takes back the default whenever
  the size it kept isn't one of the modes.

## v22: two sticks for the cryptographic sequencer; what MetalFX costs on the phone, 2026-11-08

- **v21 on the phone (the owner's log):**
  - At the limit of 60, MetalFX on: 58.6 frames a second shown of 59.8 made.
  - At 120: with MetalFX and the high resolution, 89 shown of 114 made, the GPU at 9.5 ms a frame; the medium resolution, 91 shown at 7.7 ms.
    v20 without MetalFX showed 104, the GPU at 5.5 ms. So MetalFX costs about 15 shown frames a second at 120, and nothing much at 60.
  - The processor is near its own limit at 120 too: the busiest thread spends about 7.5 ms a frame, which caps it near 130. At 60 it has
    room. Making that thread faster is the translator's work on the wish list, not a quick change.
- **Two sticks** (the owner's design): a button below the photo one, a pad with its two sticks. While it's on:
  - the stick goes to the game as the pad's left stick, in play too, instead of the keyboard's movement names;
  - a touch anywhere the camera's swipe would be is a second, floating stick, the pad's right one;
  - no running past the stick's edge; the button is lit, and stays shown outside play, so that it can always be turned off; a tap outside play
    still clicks.
  - Why the sequencer needs it: the pad's sticks feed `aRawLHJoy` and `aRawRHJoy` (DefaultInput.ini), the keyboard's WASD the left one, and the
    mouse only `aMouseX`/`aMouseY`. The swipe is the mouse, so the right stick had no way in by touch.
  - Not tried on the sequencer itself: no save at one here. The owner turns it on when the sequencer's screen comes up.
- **Anything else a playthrough needs by touch:** every pad button is on the touch pad (the four, the shoulders, the triggers, both stick clicks,
  start, back, the cross through the gadgets' wheel), and several can be held at once. The right stick's axes were the only input missing.

## v23: the processor work from branch cpu-speed, 2026-11-08

- Installed from branch `cpu-speed` (docs/CPU_SPEED.md): translated functions take guest memory as a restrict parameter of their own,
  the Objective-C bridge keeps plans for repeated messages, the hottest small library functions run in place, call sites keep their last
  target, the heap's small blocks are kept per thread, HFS path URLs are kept, sleeping threads spread over 4096 buckets. On the Mac the
  game thread went from 2.73 to 2.52 ms a frame where it is the limit.
- To compare on the phone, from v21's log at the limit of 120 and about 100 FPS: the game thread (1dab9d0) about 7.06 ms a frame, the render
  thread (1c260f0) about 8.64 ms, waiting included (the `[fps]` line's "guest threads, CPU ms a frame").
- **v23 on the phone (the owner's 277 s, limit 120, MetalFX off, high resolution):** the first minute 113-115 frames a second on screen,
  the game thread 3.6 ms a frame; from 66 s, in heavier places (up to 1,200 draws), the game thread 5.2-6.6 ms and 87-105 on screen. The
  render thread 7.1-9.0 ms all along, waiting included: it is now the one holding the frame rate. Warm from 211 s. v21 in a similar session
  at 120 (warm, then hot): the game thread 7-9 ms, 82-90 on screen. The owner felt the first minute faster and the rest about the same,
  which is the scenes getting heavier, not the heat.

## v24: the camera faster while aiming, 2026-11-08

- The cpu-speed branch is now in `ios-port` (fast-forward).
- While the touch L2 is held (a gadget aimed), the swipe turns the camera "Камера при прицеле" times faster (settings, Управление: 1 to 5,
  2.5 by default; `ARKHAM_TOUCH_AIM_LOOK`). The game itself slows the camera a lot while aiming, and the owner found aiming the batarang
  very slow.

## v25: a second throw button, one-touch combo moves, the render thread's waits measured, 2026-11-08 (never installed alone: it's in v26)

- While the touch L2 is held (a gadget aimed), a second throw button (R2, drawn as the batarang) shows on the left; the first stays. The owner
  keeps every button under the right thumb, so holding the aim left no finger for the throw.
- One touch for the moves that take two buttons at once: Special Combo Takedown (triangle and circle), Special Combo Throw (circle and cross),
  Ground Takedown (R2 and triangle). Those are the game's only two-button moves as far as known; the other upgrades are passive or use one
  button, or aim and throw.
- Crouch stays as it was (held). A tap-to-crouch was tried and taken out at the owner's word: R2 is also the throw while aiming, its release
  ends the crouch and releases a gadget, and a double tap fires the Batclaw.
- The `[fps]` line has "waiting for the layer": how long the render thread waits on the Direct3D thread a frame, to tell its own work from
  waiting. Repeated state calls are answered before the queue's full handler (no change measurable on the Mac).

## v26: the gadgets' wheel as the game's own: eight gadgets, each with its picture, 2026-11-08 (installed 14:03; v25 is in it)

- The wheel has the game's eight gadgets around the pad's cross where the game's own wheel has them: up the Batarang, up and right the Sonic
  Batarang, right the Line Launcher, down and right the Cryptographic Sequencer, down the Batclaw (later the Ultra Batclaw, the same place),
  down and left the Remote Control Batarang, left Explosive Gel, up and left several Batarangs at once. Sources: the owner's picture of the
  game's wheel, StrategyWiki's controls page (https://strategywiki.org/wiki/Batman:_Arkham_Asylum/Controls), and the game's own list of
  gadgets (Frontend.int, `[GadgetHelp]`: those eight, besides the Joker's in the challenge maps).
- The four between the cross's arms are two of its sides at once, which the PlayStation pad gives the game as its cross's diagonals
  (`read_touch` in shims_hid.m) and the Xbox pad as two buttons. The wheel's eight parts are 45 degrees each, its radius 3.4 times the button's
  (112 points at least; it was 3 times and 96 with four).
- Each gadget has its own picture in BatIcons.h (`g_*`), traced from the owner's picture of the game's wheel: each icon in a square of it,
  its outline read off a grid in the icons' 100 units and fitted to the box. The same shapes turned the same ways, solid in this port's
  manner with the game's lines cut out of them. A first set drawn from scratch (a spray can, a zip line and so on) was turned down by the
  owner: they should look like the game's. The one under the thumb is lit in the game's orange, and its name, the game's Russian one
  (Frontend.rus), shows above the wheel, or beside it where there's no room above.
- The closed gadgets button shows the pad's cross as the game's wheel has it in its middle, four keys pointing in, with the eight arrows round
  it (`bi_dpad`), for "hold and slide"; it was the batarang. The wheel opens only while the button is held, or after a tap on it, as before.
  (A picture of the open wheel, sent with a wrong caption, had made it look always open.)
- The left throw button from v25 had no icon in the icon style (its ident had none); it has the batarang now.
- The game's own wheel shows only the gadgets Batman has; this one shows all eight, and choosing one he hasn't got leaves the gadget as it was.
- Checked on the Mac with the owner's save (a frozen copy): the cross reaches the game, whose wheel comes up with "Бэтаранг" lit. That save has
  only the Batarang, so the diagonals aren't tried in the game yet: the first gadget on one (the sequencer, say) will show it.
- A scripted run's `pad:57:<hat>` steps need `ARKHAM_FAKE_PAD_TYPE=ps`: the default pad is the Xbox one, which has no hat, and the steps did
  nothing at all in the first try (a "[pad] scripted 1:57" line is logged either way).

## v27: the game's size from the inner screen even when started folded; the resolution in pixels, up to full, 2026-11-08 (installed 14:12)

- The owner saw a very low picture in v26: the game had started with the phone folded, on the outer screen (466 x 678 points), and took its
  size from it, 1016 x 696, for the whole run; unfolded, that went up to 2853 x 2007 pixels. MetalFX only scaled it up better.
- The game is now told the biggest screen this app has been on (the settings keep it: `screenW`, `screenH`, `screenScale` in main.m's
  keep_screen, updated whenever the view is laid out on a bigger one). On the foldable that's the inner screen whichever one the game starts on;
  on the outer one the picture is the inner screen's, shown smaller.
- "Разрешение игры" offers the sizes themselves for that screen: 2848×2000 (all of its pixels), 2136×1504, 1424×1000 (half, the default, as
  before) and 1064×752. It was high, medium and low (0.5, 0.42 and 0.375). Taken at the next start, as before.
- MetalFX isn't used when there's hardly anything to scale up (within 5%, as at the full size): the frame goes to the screen as it is.
- v26's log (started folded, 1016 x 696, no FPS limit): 173 frames a second made in play (the median), up to 230 and 300 to 400 in light places,
  112 to 120 shown; the GPU about 3 ms a frame, the busiest game thread 5 to 8 ms in heavier scenes. The owner calls the performance good and
  wants no more optimizing (2026-11-08).

## v28: the app's folder in the Files app, 2026-11-08 (installed 16:30)

- `UIFileSharingEnabled` and `LSSupportsOpeningDocumentsInPlace` in Info.plist: the app's Documents shows in Files (On My iPhone, "Batman
  Arkham Asylum") and in the Mac's Finder. The owner asked to see Steam's stored answer on the phone.
- In it: `Steam` (answer.txt, what the Mac's Steam answered; the saves), `Logs` (run.txt), `Shaders`, `Shots`, `arkham.env`, and
  `Batman Arkham Asylum GOTY`, the game's own files (about 9 GB), which must stay where they are.

## v29: the camera turned by the thumb holding the aim; the aim's camera up to ×15, 2026-11-09 (installed 10:32)

- **The aim thumb turns the camera.** The thumb holding the touch L2 (the gadget's aim) turns the camera as a swipe does, at the aim's speed.
  - Why: the owner keeps every button under the right thumb, so with the aim held there was no thumb left for the camera.
  - It starts once the thumb is 8 points from where it pressed, so the press itself doesn't move the aim. The travel up to there is left out.
  - The aim stays held wherever the thumb goes, until it lifts. On the map and in menus the thumb turns nothing.
- **"Камера при прицеле" goes from 1 to 15** (it was 1 to 5). The default stays 2.5.
  - A setting at the old top of 5 (the owner's) moves to 15 once (`aimCameraSpeedUpTo15`). The slider stays at its end and turns three times as fast.
  - The owner asked for exactly that, and not to measure on the Mac how much the game slows the camera while aiming.
- **Checked:** it builds without warnings. Touch can't be tried on the Mac, and the owner asked not to run the game there for this.

## v30: the screen takes touches with a controller connected, 2026-11-09 (installed 10:45)

- **What was wrong.** With a controller connected, the touch controls hid for as long as it was connected, and the screen took no touch at all,
  not even a tap on a menu's item. The owner's DualSense had connected by itself, so the game couldn't be started by touch. The game also took its
  buttons only from the controller then.
- **Now whichever was used last is the game's.**
  - While a controller is connected, the controls hide until the screen is touched.
  - A touch brings them back and works at once: a tap on a menu's item clicks it, and a touch on a button presses it.
  - Their values then go to the game as the controller's own profile, so the game sees no controller come or go (shims_hid.m).
  - The controller's next use (a button down, a trigger past 0.3, a stick past 0.4) makes it the game's again, and the controls hide.
- **Checked:** both files build without warnings, for the phone and the Mac. The Mac never turns the touch pad on, so nothing changes there.
- **Not run:** the owner asked not to run the game on the Mac this round, and touch can only be tried on the phone.
