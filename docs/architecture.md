# Architecture

## The game

Monster Truck Madness 2 (Terminal Reality for Microsoft, 1998) is one PE image,
`Monster.exe` (2.9 MB, linker 3.10, built 1998-07-14). The CRT and MFC are
linked in statically, the image is incrementally linked (a 4,623-slot thunk
table at `0x401000`), and there is no protection.

| File | What it is |
|---|---|
| `Monster.exe` | The game. MFC front end drawn with GDI; DirectDraw for the videos and the software renderer; DirectSound with its own mixer thread |
| `SMACKW32.DLL` | RAD Smacker, for the `.smk` videos. Runs natively |
| `TRID3D.DLL`, `TRIGLIDE.DLL`, `TRINEC.DLL`, `TRIREND.DLL` | Hardware renderers (Direct3D, Glide, PowerVR, Rendition), picked by `rendererDLLPath` in `system\monster.ini`. Not loaded in software mode, which is the default |
| `VOXRT24.DLL` | Voxel runtime. Not loaded so far |
| `*.POD` | Terminal Reality's archives, listed in `POD.INI` and opened by bare name from the current folder |
| `system\monster.ini` | Every setting, read with `GetPrivateProfile*` |
| `VIDEOS\`, `Shared\Videos\` | Smacker videos. The intros are `Shared\Videos\MSLogo.smk` and `TRI.smk` |

The disc keeps the game in `ENGLISH\` and `SHARED\`. `tools/gamefiles.py`
merges them into `game\` as the installer did, but keeps `SHARED\VIDEOS` as
`Shared\Videos`, the path the game opens.

## The pipeline

```
disc / ISO ── tools\gamefiles.py ── game\Monster.exe ── tools\catalog.py ── work\functions_monster.json
              (pcrecomp iso_peek)                        (pcrecomp disasm32)          │
                                                                                run_lift.py
                                                                    (pcrecomp generate + lift32)
                                                                                      │
src\runtime\host.c + ddraw.c + present.c + pcrecomp native32 ── build ── src\recomp\gen\recomp_00NN.c
                                                                  │
                                                            build\mtm.exe
```

1. **`tools/catalog.py`** runs pcrecomp `disasm32`: 22,278 functions, 81.0%
   of `.text`. Addresses that a run finds only at run time go in
   `work/seeds.json` through pcrecomp `seed_from_log.py`. None are needed today.
2. **`run_lift.py`** lifts every catalogued function into one dispatch table
   (same driver as Virtual Springfield's).
3. **The build** compiles the lift, the host and pcrecomp's `native32` into one
   32-bit exe linked at `0x60000000`, so the guest's `0x00400000` is free.

## At run time

`mtm.exe` maps `Monster.exe` at its own base, with its code non-executable,
and runs only lifted code. `native32` bridges guest calls to Windows and
Windows callbacks (the window procedure, the mixer thread) back into lifted
code.

What the host changes about the game's world (`src/runtime/host.c`, `ddraw.c`):

- **DirectDraw is the host's own** (`ddraw.c`, from Virtual Springfield):
  surfaces in memory, and an exclusive 640x480x16 mode that is only
  remembered. The game's display mode never reaches the real display.
- **`ChangeDisplaySettingsA` is never applied.** The front end does not import
  it. It fetches it with `GetProcAddress` (`sub_0058B310`) and switches the
  desktop to 640x480 and back as its window gains and loses focus. With a
  cloaked headless window that loses focus constantly, the real desktop
  flipped modes over and over. The host's `GetProcAddress` hands back a stub
  that logs `[display] ... not applied`.
- **The front end is GDI**, and so is the race in its default windowed mode
  (a DIB section `StretchBlt` to the window). DirectDraw only carries the
  intro videos. Recording reads the window back with `PrintWindow`, which
  works on a cloaked window too, from the recorder's own thread, and only
  while the UI thread is idle. The host puts its own `GetMessageA` and
  `PeekMessageA` in the guest's IAT to know when that is: with an empty
  queue every control has repainted. Read at any other moment, it caught the
  menu between erasing its controls and repainting them. Called on the UI
  thread, inside a message sent from another thread, `PrintWindow` never
  returned.
- **Every top-level window is cloaked when headless.** GO opens a modal MFC
  dialog ("Before You Begin"), made with `CreateDialogIndirectParamA`, which
  the `CreateWindowExA` shim never sees. A CBT hook on the game thread cloaks
  each one as it is created. MFC centres such a dialog on the desktop rather
  than on the cloaked main window, so it opened on another monitor; when it
  activates off the main window the hook centres it there. The recorder
  draws the active popup over the main window, and `--click` goes to it.
- **Keys go to the focus window.** The race is a view inside an MDI frame,
  and reads its keys from window messages: posted to the frame they did
  nothing. `--key` posts to the game thread's focus window
  (`GetGUIThreadInfo`), which is the race's view, or a dialog's control, as
  a real key would be. `GetKeyState` sees the keys `--key` holds.
  DirectInput is created, but no device is made on it.
- `smackw32.dll` and DirectSound run natively through the bridge.
