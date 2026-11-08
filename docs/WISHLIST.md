# What the owner has asked for, and where each stands

Kept so that a wish isn't lost between rounds of fixes; updated with each version. Newest state first in each list.

## Open

- **The startup's black screen** (about 8.5 s from tapping the icon to the title on the phone, from the user's screen recording and the log, 2026-11-08):
  - 2.5 s of it were libunwind's `unw_resume` after the game's one startup throw, replaced in v20 (unverified on the phone).
  - About 4 s of engine start, much of it `stat`/`lstat` on files that aren't there: about 55 000 in the first 20 s, each answered from
    the path memory and then asked of the system anyway. Answering "not there" from the memory would save part of it.
  - A picture or the Batman logo instead of black while it loads, if wanted.
- **Frames at the limit of 60 evenly on the phone, also after turning or folding it.** v20 keeps one layer, holds each frame to the display's
  own beat (a display link at the limit's rate), and stops the controls asking the display for 120. v19 still showed 1 or 2 frames a second
  missing in a third of its seconds.
- **The inner screen upright both ways.** iOS refuses upside-down portrait on iPhone whatever the app allows, and on the foldable's inner screen
  that is one of the two upright ways (v19's log). Only the game turning itself could do it.
- **The gadgets' button showing the gadget chosen** (the game's choice isn't known outside it yet).
- **Rumble, Mac and phone** (the owner confirmed it's wanted, 2026-11-08, for later). Nobody knows yet whether it works on the Mac: the
  haptics engine is retried there since 2026-11-07, never felt with a pad connected. Then the phone: Feral's code goes through IOHIDDevice and
  ForceFeedback, which has to become GameController haptics.
- **MetalFX, temporal.** The spatial upscaler is in since v21. Temporal needs jitter and motion vectors; UE3 makes no motion vectors.
- **The cryptographic sequencer by touch:** two sticks since v22 (a button, the owner's design). To confirm on the phone at the first sequencer.
  Telling by itself when the sequencer is up isn't done: the button is turned on by hand.
- **Low Power Mode** (2026-11-08): with it on, the phone made about 50 frames a second at the limit of 60 and showed 46 (v20's log).
  iOS lowers the processor's clock and the game's threads can't keep up. Possible: say so in the frame rate line when it's on.
- **MSAA as a setting.** The game's resolution is one since v21 (high, medium, low: 0.5, 0.42, 0.375 of the screen's pixels).
- **Better anti-aliasing and upscaling:** FXAA with the game's MSAA (FXAA is in), MetalFX (spatial first; temporal needs jitter and motion
  vectors), TAA, possibly SMAA.
- **Speed:** the 300 FPS bar on the Mac (the Windows version gives about 350), game thread first, a second-generation translator in its own
  branch. On the phone (v17's profile) the render thread spent 90% of play waiting on occlusion query polls, one Direct3D thread wake-up each.
  Answering them itself (v18) ran it ahead in bursts and dropped frames, so that is `ARKHAM_QUERY_FAST=1` only. At 120 the busiest thread
  spends about 7.5 ms a frame (v21's log), so the processor caps the phone near 130; the owner asked for processor work (2026-11-08) and chose
  to play first.
- **MetalFX cheaper at 120:** it costs about 15 shown frames a second there (v21: 89 to 91 shown against 104 without; the GPU at 9.5 ms against
  5.5). Possible: scale up to less than all of the screen's pixels, or turn it off by itself above 60.
- **Hitches** of 45 to 85 ms a few times in 5 minutes on the phone, mostly where new shaders first appear. The user had said earlier not to
  work on shader compiling further.
- **Mac:** play the `door-stutter` branch (a5188d3) by hand, then merge it into `first-frame`. Also check the elevator lights, a change of
  resolution (`Device.Reset` never ran), and whether that brings back the 62 FPS limit as on Windows.

## Done, by version (phone)

- v22: two sticks for the cryptographic sequencer (a button; the stick and a second floating stick as the pad's two).
- v21: MetalFX spatial upscaling to the screen's pixels (on by default); the game's resolution as a setting.
- v19 to v20: the occlusion query change made opt-in; the startup throw's resume done by the runtime itself; each turn of the phone logged.
- v17 to v18: the map by touch (a drag moves it as far as the finger, two fingers zoom, a "Показать Бэтмена" button); a soft dark ring under
  each icon; the badges fade with their button; a new counter (shield) and fist; the picture-following colour removed; all four ways up
  allowed.
- v16: even frames at a limit (each held for the limit's step); the PlayStation badges show; no startup films.
- v15: own icons in the batarang's manner; white buttons by default; the menus' cursor hidden but working; the map pans; the Metal HUD switch.
- v14: the camera turns while walking and running (walking through the keyboard's movement names).
- Earlier: touch controls with a layout editor (move, resize, hide or reset each button, reset all with a question), transparency,
  hiding outside play, photo mode, settings as a table, the frame rate limit, running on ✕, PlayStation and Xbox styles, the phone upright.
