# PPWidescreenFix v1.1

A native-resolution plugin for **Pink Panther: Pinkadelic Pursuit (2002)**,
v1.0 ENGLISH No-CD/Fixed EXE.

## What this does

Hooks/patches the game at four points to let it run at a resolution other
than the hardcoded 640x480:

1. **`AdjustWindowRectEx`** (user32.dll) — the main fix. Rewrites the
   client rect the game asks for from 640x480 to your configured target
   *before* the window/backbuffer/display-mode chain runs, so everything
   downstream inherits the new size automatically.
2. **`AGLSCREEN::SetViewport`** (Agl7.dll) — forced to always cover the
   full new resolution, as a safety net.
3. **`AGLSCREEN::SetDisplayMode`** and **`AGLSCREEN::CreateImage`**
   (Agl7.dll) — hooked for logging/diagnostics and a fallback override.
4. **NEW in v1.1 — the "sky rectangle" patch.** A direct, verified
   in-memory byte patch for a second hardcoded 640x480 pair that lives
   *inside the exe itself* (not inside Agl7.dll), which builds a colored
   background rectangle sized to the original resolution. Without this,
   everything outside that rectangle stays black at larger resolutions.
   Unlike the three hooks above (which target Agl7.dll by function name,
   so they survive a different exe build), this one is tied to the exact
   v1.0 ENGLISH No-CD/Fixed build by raw address — the plugin checks the
   original bytes match exactly before writing anything, so on a
   different exe it safely does nothing rather than corrupting something.

Full technical detail (addresses, call chains, why each piece works) is in
the comments at the top of `PPWidescreenFix.cpp`.

**Known limitation:** this still does not rescale the game's own HUD/menu
2D draw calls — those are a separate, much larger reverse-engineering
effort. Expect HUD/gameplay content to still render at original size
inside the bigger frame, even with this update.

---

## Part 1 — Building it (no local install needed)

No Visual Studio, no local toolchain — this zip includes a GitHub Actions
workflow (`.github/workflows/build.yml`) that builds `dinput8.dll` for you
in the cloud, for free, using GitHub's own Windows build servers.

### Steps
1. On GitHub, create a **new repository** (Settings don't matter much —
   public or private both work; if private, Actions minutes are still
   free for individual accounts at this scale).
2. Upload every file from this zip into it, **keeping the folder
   structure intact** — specifically, `.github/workflows/build.yml` has
   to end up at that exact path in the repo, not flattened into the root.
   The easiest way: on the repo's page, use **Add file → Upload files**,
   and drag the whole extracted `PPWidescreenFix` folder's contents in —
   GitHub preserves subfolder paths from drag-and-drop.
3. Go to the **Actions** tab of your new repo. GitHub will likely ask you
   to confirm enabling Actions for the repo the first time — accept that.
4. Click into the **"Build PPWidescreenFix"** workflow on the left, then
   click **"Run workflow"** (a button on the right side) → **Run workflow**
   again to confirm. This starts the cloud build.
5. Wait ~1-2 minutes, then click into the run that appears. If it
   finishes with a green check, scroll down to **Artifacts** and download
   **`dinput8-dll`** — unzip that one small zip and you have your real,
   compiled `dinput8.dll`.
6. If it fails (red X), click into the failed step to see the error, and
   send me that text — I can fix the workflow or the source from the
   error message even though I can't run the build myself.

### Alternative: building it locally instead

If you'd rather build it yourself on your own machine instead of using
GitHub Actions, that's also fully supported — Visual Studio Community
edition is free:

### Requirements
- Visual Studio 2022, Community edition (free) — install the
  **"Desktop development with C++"** workload during setup.
- [MinHook](https://github.com/TsudaKageyu/minhook) — download the repo
  as a ZIP (green "Code" button → "Download ZIP") and extract it anywhere.
  Note: MinHook doesn't ship a ready-made `.lib` file — we compile its
  4 source files directly alongside ours below, which avoids needing to
  build it as a separate step first.

### Steps
1. Open Visual Studio → **Create a new project** → search "dll" →
   **Dynamic-Link Library (DLL)** (the C++ one) → Next → name it anything
   (e.g. `PPWidescreenFix`) → **Create**.
2. Visual Studio generates a project with one auto-created `.cpp` file
   (visible in **Solution Explorer** under **Source Files**, named after
   your project). Double-click it to open it, select everything inside
   (**Ctrl+A**), delete it, and paste in the **entire contents** of this
   zip's `PPWidescreenFix.cpp` instead. Save (**Ctrl+S**).
3. Add MinHook's source files to the project: in **Solution Explorer**,
   right-click **Source Files → Add → Existing Item**, browse to where
   you extracted MinHook, and add these three files from its `src`
   folder: **`buffer.c`**, **`hook.c`**, **`trampoline.c`** — then from
   its `src\hde` folder, add **`hde32.c`** (not `hde64.c`). You should
   now have 5 source files total in the project.
4. Set the platform to **x86**: at the top toolbar there's a platform
   dropdown (often showing `x64`). Click it → if `x86` isn't listed,
   click **Configuration Manager...** → **Active solution platform**
   dropdown → **\<New...\>** → choose **x86** → **OK** → **Close**. Make
   sure the platform dropdown now shows **x86**, and the configuration
   dropdown next to it shows **Release**.
5. Right-click the project in Solution Explorer → **Properties**. At the
   top of the dialog, set **Configuration: All Configurations** and
   **Platform: x86** (important — settings only apply to whichever
   combination is selected here), then:
   - **C/C++ → General → Additional Include Directories** → Edit → add
     three paths, one per line: the MinHook folder's `include`, `src`,
     and `src\hde` (e.g. `C:\minhook-master\include`,
     `C:\minhook-master\src`, `C:\minhook-master\src\hde`).
   - **General → Target Name** → change to exactly `dinput8`.
   - Click **OK**.
6. Build: **Build → Build Solution** (or `Ctrl+Shift+B`). Watch the
   **Output** window at the bottom for errors.
7. If it succeeds, the **Output** window's last lines show the exact
   path — normally `<project folder>\Release\dinput8.dll`.

---

## Part 2 — Installing it in the game folder

1. In your Pink Panther install folder, find the existing `dinput8.dll`.
   - If there is one, **rename it to `dinput8_orig.dll`**.
   - If there isn't one, copy `C:\Windows\SysWOW64\dinput8.dll` into the
     game folder and rename *that* copy to `dinput8_orig.dll`.
2. Copy the `dinput8.dll` you just built into the game folder (next to
   `PPanther.exe` / `PPanther-NoCD.exe` and `Agl7.dll`).
3. Launch the game normally.
4. Two new files should appear next to the exe:
   - **`PPWidescreenFix.ini`** — edit this to set your resolution (see
     below). No rebuilding needed to change it.
   - **`PPWidescreenFix.log`** — shows exactly what each hook/patch did
     on that run. Check this first if anything looks wrong.

## Configuration (`PPWidescreenFix.ini`)

```ini
[Settings]
Width=1920
Height=1080
ForceFullViewport=1
LogResourceCalls=0
```

Change `Width`/`Height` to whatever you want to test — the sky-rect patch
and all the hooks read these same values, so everything stays in sync.

## Checking it worked

Open `PPWidescreenFix.log` after a run. You should see lines like:
```
Hooked AdjustWindowRectEx
Hooked AGLSCREEN::SetViewport
Hooked AGLSCREEN::SetDisplayMode
Hooked AGLSCREEN::CreateImage
[Patch] sky-rect height: 0x1E0 -> 0x438 at RVA 0x20607
[Patch] sky-rect width: 0x280 -> 0x780 at RVA 0x2060C
```
If you instead see a `SKIPPED` line for the sky-rect patch, it means the
exe you're running has different bytes than expected at that address —
most likely a different version/language build than v1.0 ENGLISH
No-CD/Fixed. The rest of the plugin (the Agl7.dll hooks) will still work
normally either way.

---

## Important: if you're also using DDrawCompat

You're currently running DDrawCompat with `DisplayAspectRatio=4:3`, which
tells DDrawCompat to force a 4:3 picture (with pillarboxing) regardless of
what the game asks for — that made sense when the game only ever
requested 640x480 internally.

Once this plugin is active and actually making the game request a wider
resolution (e.g. 1920x1080) internally, keeping `DisplayAspectRatio=4:3`
in DDrawCompat would work *against* this fix — it would re-pillarbox an
image the game is now genuinely trying to render wide. At that point you
likely want to change your `DDrawCompat.ini` to:
```ini
DisplayAspectRatio = app
```
so DDrawCompat just presents whatever aspect ratio the game is now
actually asking for, instead of continuing to force 4:3 on top of it.

---

## Build order for testing

1. Build and install as above, confirm the log shows all four
   hooks/patches succeeding.
2. Launch at your configured resolution — expect it to boot without
   crashing, and the black gap outside the old 640x480 area should now be
   filled by the sky-rect fix instead of staying black.
3. Update the DDrawCompat setting above if you haven't already.
4. HUD/gameplay still rendering at original size in a corner is expected
   — that's the known limitation, a separate piece of work.
