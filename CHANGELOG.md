# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow [SemVer](https://semver.org/).

## [Unreleased]

### Added
- Monster Truck Madness 2 lifted whole with pcrecomp (22,279 functions, 0 lift
  errors) and run on the `native32` host: it boots, plays the Smacker intros
  and reaches the title menu. Needs pcrecomp #81.
- `tools/gamefiles.py` (game files off the disc or ISO), `tools/catalog.py`,
  `run_lift.py`, the host (`src/runtime/`), and a netlab recipe (`projects/mtm.env`).
- Headless recording of the GDI front end and the windowed race, read back
  with `PrintWindow` while the UI thread is idle, with the active dialog drawn
  over it.
- Headless runs cloak every top-level window (the front end's dialogs too),
  and `--key`/`--click` reach a modal dialog and the MFC child controls under
  the point. With these, a scripted run reaches a race on the software
  renderer.

### Fixed
- The game's `ChangeDisplaySettingsA`, fetched with `GetProcAddress`, no
  longer changes the real desktop's mode. It was switching it over and over
  during headless runs.
