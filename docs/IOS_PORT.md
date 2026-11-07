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

## Decisions for the user

- **Steam.** I won't make the game believe Steam said yes when it didn't. What I'd build instead: the phone asks the real Steam running on the user's Mac, over the local
  network, through a small helper on the Mac that uses Valve's own library, and the helper can carry save files both ways too. The Mac with Steam then has to be on the
  same network while playing.
- **Apple developer account.** With the free Personal Team the app has to be installed again every 7 days and can't have the larger memory limit; the paid account
  ($99 a year) installs for a year and allows it.
- **Metal Toolchain**, for building shader libraries on the Mac: a download through Xcode.
