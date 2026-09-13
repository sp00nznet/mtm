# Monster Truck Madness — Static Recompilation

Static recompilation of **Monster Truck Madness** (Microsoft / Terminal
Reality, 1996) and **Monster Truck Madness 2** (1998) from their shipping Win32
binaries to native C.

Built on the [pcrecomp](https://github.com/sp00nznet/pcrecomp) toolchain, and
directly on [fury3](https://github.com/sp00nznet/fury3) and `hellbender` — same
engine house, two years later.

## Project Status: **P0 complete, P1 not started**

---

## Why this one

Terminal Reality's engine is already the most thoroughly measured thing in the
collection. Fury³ (1995) and Hellbender (1996) are the same engine twice, and
the toolkit's own notes say why that mattered: *anything that works on Fury³
and fails on Hellbender is a tool bug, not a game quirk*. That pair settled the
carry-flag model in `lift32.py` by measurement.

Monster Truck Madness 2 is the same engine a third time, two years on, and it
ships the renderer as separate named DLLs instead of linking it in:

- `VOXRT24.DLL` (248 KB, built 1997-08-13) — the voxel runtime, the direct
  descendant of what is statically linked inside Fury³ and Hellbender
- `TRID3D.DLL` (81 KB) — **T**erminal **R**eality **I**nteractive Direct3D

So the part of the engine that Fury³ forced the lifter to get right is here as
a standalone 248 KB DLL with an 8-function export table. That is a far better
regression target than a 1.9 MB game binary with the renderer buried in it.

`nocturne` (1999) is the fourth Terminal Reality generation and is already in
the collection, which makes this family: 1995, 1996, 1998, 1999.

---

## What P0 found

| Binary | Size | `.text` | Linker | Built | Imports |
|--------|-----:|--------:|--------|-------|---------|
| `Monster.exe` (MTM2) | 2,925,568 | 2,119,125 | 3.10 | 1998-07-14 | DDRAW, D3D via TRID3D, DSOUND, DINPUT, DPLAYX, SMACKW32, MSACM32 |
| `VOXRT24.DLL` | 248,080 | — | 5.0 | 1997-08-13 | MSVCRT, KERNEL32 (8 exports) |
| `TRID3D.DLL` | 80,896 | — | 3.10 | 1998-03-25 | KERNEL32, USER32, DDRAW (27 exports) |

**No DRM, no packer, no protection of any kind.** Entropy 4.13–4.97 across
every section — this is plain, unobfuscated 1998 code.

Two things worth noting for P1:

- **The CRT is statically linked.** `Monster.exe` imports no `MSVCRT.dll`,
  which means the CRT startup and every library call is inside the 2.1 MB of
  `.text` and has to be classified out before the game code is visible.
  `tools/classify/` exists for exactly this — it is what proved 78% of Gunman
  Chronicles was SDK.
- **`.data` has a 7 MB virtual size against 128 KB on disk.** A 6.9 MB
  zero-filled BSS. Nocturne is the reason `image_loader.c` clamps its section
  copy; this is the shape of binary that found that bug, so the clamp will get
  a second workout here.
- The debug directory is present but stripped — three entries, all type 0 with
  null pointers. No CodeView, no COFF symbols. Do not go looking.

`Monster.ex_` on the disc is not compressed despite the name — it is a plain
PE. It is only the Microsoft ACME installer's naming convention.

Monster Truck Madness 1 (1996) ships through the ACME setup (`ACMSETUP.EXE` +
compressed cabinets) and has not been unpacked yet.

---

## Where it goes next (P1)

1. `disasm/disasm32.py` over `VOXRT24.DLL` first. Small, self-contained, and
   directly comparable against the voxel code already lifted in `fury3`.
2. Classify the CRT out of `Monster.exe` before disassembling it.
3. Unpack MTM1's ACME cabinets and check whether its renderer is a separate DLL
   too, or still linked in like Fury³'s.

---

## Layout

```
mtm/
  original/   both discs (MTM2 .iso, MTM1 .bin/.cue and the .iso from it)
  game/       extracted binaries
  analysis/   P0 output
  docs/
```

## Credits

Monster Truck Madness © 1996 and Monster Truck Madness 2 © 1998, Microsoft /
Terminal Reality. This project neither contains nor distributes any part of
them.
