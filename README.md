# PPWidescreenFix v1.5

Native resolution / **true widescreen** plugin for **Pink Panther: Pinkadelic Pursuit (2002)**
(v1.0 ENGLISH No-CD/Fixed EXE). Loads as a `dinput8.dll` proxy; works alongside
[DDrawCompat](https://github.com/narzoul/DDrawCompat) and
[ogg-winmm](https://github.com/ayuanx/ogg-winmm) (they proxy other DLLs).

## How the game renders (verified)
- `Agl7.dll` creates a **Direct3D 7 HAL device** on the DirectDraw back buffer
  (`IDirect3D7::CreateDevice`, `IID_IDirect3DHALDevice`) and stores it at `AGLSCREEN+0x160`.
- The exe draws everything with `DrawPrimitive` using **pre-transformed `XYZRHW` vertices**
  (FVF `0x44` / `0x144`): HUD, font glyphs, menu backdrops, the sky rectangle and the 3D
  world (projected in software: focal length = constant 479.0, centre = viewport size / 2,
  so a wider canvas simply shows more to the sides = Hor+).
- Layout code reads the screen size from a context struct (exe VA `0x443d44`, +8 width,
  +0xC height). Text is centred on width/2, some HUD items are anchored to width - N, others
  sit at fixed positions on the left.
- The game's D3D viewport is always the full context rectangle. The game does **not** clip
  polygons itself: polygons that stick out of the screen (x from about -580 to +1200) were cut
  off by the viewport.

## What v1.5 does
Lays everything out on a **virtual canvas**, clips every pre-transformed primitive to that
canvas on the CPU, then scales it to the real surface in the `DrawPrimitive` hook.

| `Mode` | Virtual canvas | Result |
|---|---|---|
| `1` (default) | 853x480 at 16:9 | **true widescreen** (Hor+), fills the whole screen, no bars |
| `2` | 640x480 | the original game, centred with black bars (reference / fallback) |
| `0` | none | resolution patch only (v1.2 behaviour) |

Full-screen 4:3 pictures (loading / title / menu art): `BackdropFit=1` stretches them to the
screen (no bars); `BackdropFit=0` keeps them 4:3, centred, with black bars.

### Changes since v1.3
- **Clipping.** v1.3 forced the real viewport to the whole surface, so geometry that the
  original viewport used to cut off was drawn outside the picture. Now every triangle/quad is
  clipped to the virtual canvas (perspective-correct texture coordinates).
- **Early virtual size.** The plugin overwrites the exe's screen-size struct at the first
  Direct3D call after the exe stored the real size, so nothing is built with real-size
  coordinates (the loading / title backdrops were). A safety net also normalises any
  full-screen quad that still has real-size coordinates.
- **Mode 1 fills the screen exactly** (no sliver at the edges), any aspect ratio.

### `PPWidescreenFix.ini`
| Key | Default | Meaning |
|---|---|---|
| `Width`, `Height` | 1920, 1080 | real resolution |
| `Mode` | 1 | 1 widescreen / 2 pillarbox / 0 off |
| `BackdropFit` | 1 | mode 1: 1 = stretch full-screen pictures, 0 = keep 4:3 with bars |
| `ClipToCanvas` | 1 | 0 = v1.3 behaviour (for comparison) |
| `Recon` | 1 | write diagnostic lines to `PPWidescreenFix.log` |

## Install
1. Build `dinput8.dll` with GitHub Actions (Actions -> *Build PPWidescreenFix* -> Run workflow
   -> download the `dinput8-dll` artifact).
2. Game folder: real `dinput8.dll` renamed `dinput8_orig.dll`, plugin as `dinput8.dll`.
3. Run once; edit the generated `PPWidescreenFix.ini`.

## DDrawCompat notes
- Don't force `DisplayAspectRatio=4:3`; leave it at `app`.
- The game now renders at your real resolution, so `ResolutionScale=app(3)` means 3x that
  (5760x3240 at 1080p) - try `app(1)` or `app(2)`.

## Testing procedure
1. `Mode=1` (default): menu, loading screen, gameplay (indoor + outdoor), pause menu. Check
   the HUD corners, the edges of the picture (anything missing / popping in at the sides) and
   that nothing is drawn outside the screen.
2. If something looks wrong, try `Mode=2` for comparison (original 4:3 layout).
3. Send screenshots + `PPWidescreenFix.log` (the log is overwritten on every launch - rename it
   between runs).

## Known limitations
- Several patches use addresses of this exact exe build; they are verified before writing.
- Mode 1 is new: objects the game culls with its own 4:3 assumptions could pop in at the
  sides, and HUD items with fixed positions stay where the original put them (not tested yet).
- FMV (movie) playback is not touched.
- 25 FPS cap: located (busy-wait in the per-frame function `0x402a90`, tick `[0x444168]` vs
  interval `[0x443d30]`) but not changed yet.
