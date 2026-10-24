# PPWidescreenFix v1.3

Native resolution / widescreen plugin for **Pink Panther: Pinkadelic Pursuit (2002)**
(v1.0 ENGLISH No-CD/Fixed EXE). Loads as a `dinput8.dll` proxy; works alongside
[DDrawCompat](https://github.com/narzoul/DDrawCompat) and
[ogg-winmm](https://github.com/ayuanx/ogg-winmm) (they proxy other DLLs).

## How the game renders (verified)
- `Agl7.dll` creates a **Direct3D 7 HAL device** on the DirectDraw back buffer
  (`IDirect3D7::CreateDevice`, `IID_IDirect3DHALDevice`) and stores it at `AGLSCREEN+0x160`.
- The exe draws through it with `DrawPrimitive` using **pre-transformed `XYZRHW` vertices**
  (FVF `0x44` / `0x144`): HUD, font glyphs (one 15x30 quad each), menu backdrops, the sky
  rectangle, and the 3D world (projected in software). The game never calls
  `SetTransform(PROJECTION)`.
- Layout code reads the screen size from a context struct (exe VA `0x443d44`, +8 width,
  +0xC height) - 77 read sites. UI uses a mix of fixed 640x480-space constants and that size.

## What v1.3 does
Makes the exe lay everything out on a **virtual canvas**, then scales all pre-transformed
vertices to the real surface in the `DrawPrimitive` hook, and forces the real Direct3D
viewport to the full surface. Because everything (HUD, menus, 3D projection) is computed in
the same virtual space, it stays consistent.

| `Mode` | Virtual canvas | Result |
|---|---|---|
| `0` | none | v1.2 behaviour (resolution patch only) |
| `1` | e.g. 853x480 | **true widescreen** (Hor+). 4:3 backdrops (menu/loading art) stay 4:3 if `Keep43Backdrops=1` |
| `2` | 640x480 | the original layout, scaled and centered with black bars (safe baseline) |

## Install
1. Build `dinput8.dll` with GitHub Actions (Actions -> *Build PPWidescreenFix* -> Run workflow
   -> download the `dinput8-dll` artifact).
2. Game folder: real `dinput8.dll` renamed `dinput8_orig.dll`, plugin as `dinput8.dll`.
3. Run once; edit the generated `PPWidescreenFix.ini` (see `PPWidescreenFix.ini` here).

## DDrawCompat notes
- Don't force `DisplayAspectRatio=4:3`; leave it at `app`.
- The game now renders at your real resolution, so `ResolutionScale=app(3)` means 3x that
  (5760x3240 at 1080p) - try `app(1)` or `app(2)`.

## Testing procedure
1. `Mode=2`: check menu, a loading screen and gameplay. Expect the original game, centered.
2. `Mode=1`: same screens. Send screenshots + `PPWidescreenFix.log` (the log is overwritten
   on every launch - rename it between runs).

## Known limitations
- Several patches use addresses of this exact exe build; they are verified before writing.
- Elements designed to be centered with fixed 640-based constants may sit left of center in Mode 1.
- 25 FPS cap: located (busy-wait in the per-frame function `0x402a90`, tick `[0x444168]` vs
  interval `[0x443d30]`) but not changed yet.
