// ============================================================================
// PPWidescreenFix v1.3 (virtual canvas) - Native resolution plugin for
//   Pink Panther: Pinkadelic Pursuit (2002)
//   v1.0 ENGLISH No-CD/Fixed EXE
//
// v1.1 CHANGE: added a direct, verified in-memory patch for a second
// hardcoded 640x480 pair found inside the exe itself (not inside Agl7.dll),
// which constructs a colored background/"sky" rectangle entity sized to the
// original resolution. At larger resolutions this left an uncleared black
// gap outside that rectangle. See "PATCH: sky/background rectangle entity"
// below for full details, including why this one single patch below is
// tied to this exact exe build in a way the other hooks are not.
//
// HOW IT WORKS
// ------------
// This game's window/backbuffer resolution is decided ONCE, very early in
// startup, by a single hardcoded call:
//
//      CreateGameWindow(hInstance, "Pink Panther...", 640, 480, 1);
//
// which internally does, in order:
//   1. AdjustWindowRectEx(&rect{0,0,640,480}, style, ...)   [user32.dll]
//   2. CreateWindowExA(..., adjustedWidth, adjustedHeight, ...) [user32.dll]
//   3. AGLCreateScreen(hwnd, bpp)                           [Agl7.dll]
//   4. GetClientRect(hwnd, &rect)                           [user32.dll]
//   5. AGLSCREEN::SetDisplayMode(rect.right-rect.left,
//                                 rect.bottom-rect.top, 16, 0) [Agl7.dll]
//
// Rather than binary-patching the .exe (which only affects that one copy
// of the file and can't be made user-configurable), this plugin hooks
// AdjustWindowRectEx and rewrites the *client rect* the game asks for
// BEFORE that whole chain runs. Everything downstream (CreateWindowExA,
// AGLCreateScreen, GetClientRect, SetDisplayMode) naturally inherits the
// new size, because it all reads back from the real window/rect rather
// than reusing the original 640/480 literals.
//
// We additionally hook AGLSCREEN::SetViewport to force it to always cover
// the full new resolution (defensive - in the retail binary this already
// derives from GetClientRect, so this hook is a safety net more than a
// strict requirement).
//
// We do NOT hook anything "camera"-related, because this engine has no
// 3D camera or projection matrix: the executable only imports AGL's 2D
// (AGLSCREEN/AGLIMAGE) functions, never the engine's 3D layer
// (AGLUViewMatrix / AGL3DITEM / AGLNODE, which exist inside Agl7.dll but
// are never called by this particular game). This title is pure 2D
// sprite/DirectDraw blitting.
//
// KNOWN LIMITATION (not fixed by this plugin)
// ---------------------------------------------
// The game's own 2D draw calls (backgrounds, HUD icons, menu text) are
// written against a fixed 640x480 (or in some places 640x256-ish texture
// page) coordinate space *inside the executable's own code*, not inside
// Agl7.dll. Since those coordinates are baked into the exe's compiled
// code rather than routed through any Agl7.dll export, a DLL that only
// hooks Agl7.dll cannot rewrite them. Expect:
//   - Backgrounds/HUD to appear scaled/positioned for 640x480 inside the
//     larger frame (small, or pinned to a corner).
//   - Possible uncleared/black regions where a fixed-size fill/clear call
//     (also internal to the exe) doesn't cover the new larger canvas.
// Fixing that requires patching the exe's internal draw routines
// directly (a separate, larger effort) - this plugin only fixes the
// resolution/backbuffer/viewport/window layer.
//
// CONFIGURATION
// -------------
// Edit PPWidescreenFix.ini (created next to this DLL) to set your
// target resolution. No recompilation needed to change resolution.
//
// BUILD / INSTALL
// ----------------
// See README.md in this folder for full build and install instructions.
// ============================================================================

#include <windows.h>
#include <stdio.h>
#include "MinHook.h"

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
static int  g_targetWidth   = 1920;
static int  g_targetHeight  = 1080;
static bool g_forceViewport = true;
static bool g_logResources  = false;
static bool g_logToFile     = true;
static bool g_recon         = true;   // log what the game sends to Direct3D (no behaviour change)

// --- v1.3 "virtual canvas" ---------------------------------------------------
// The exe lays EVERYTHING out (HUD, menus, full-screen quads, 3D projection) from
// two ints it stores right after SetDisplayMode (struct at exe VA 0x443d44,
// +8 = width, +0xC = height). We overwrite those with a VIRTUAL size and then,
// in the DrawPrimitive hook, scale every pre-transformed vertex up to the real
// surface. Everything stays internally consistent.
//   Mode 0 = off (v1.2 behaviour)
//   Mode 1 = virtual canvas with the screen's aspect ratio, e.g. 853x480 (TRUE WIDESCREEN)
//   Mode 2 = virtual 640x480 canvas, centered with bars (PILLARBOX, the original layout)
static int   g_mode      = 2;
static bool  g_keep43    = true;     // mode 1: full-screen textured images (menus/loading) stay 4:3
static int   g_virtW     = 640, g_virtH = 480;
static float g_scale     = 1.0f, g_offX = 0.0f, g_offY = 0.0f;
static float g_off43     = 0.0f, g_offY43 = 0.0f, g_scale43x = 1.0f, g_scale43y = 1.0f;
static bool  g_clearBars = false;

static const int ORIG_CLIENT_WIDTH  = 640;
static const int ORIG_CLIENT_HEIGHT = 480;

static FILE* g_log = nullptr;

// defined in the RECON section below; the AGLSCREEN hooks call it
static void TryHookDevice(void* screen);
static void ApplyVirtualScreen();
static void ClearBackBuffer(void* screen);

static void LogInit()
{
    if (g_logToFile)
        fopen_s(&g_log, "PPWidescreenFix.log", "w");
}

static void Log(const char* fmt, ...)
{
    if (!g_log) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fflush(g_log);
}

static void ComputeVirtualCanvas()
{
    float W = (float)g_targetWidth, H = (float)g_targetHeight;
    g_virtH = 480;
    if (g_mode == 1)
    {
        g_scale = H / 480.0f;
        g_virtW = (int)(480.0 * g_targetWidth / g_targetHeight + 0.5);
        if (g_virtW < 640) g_virtW = 640;
        g_offX = (W - g_virtW * g_scale) * 0.5f;
        g_offY = 0.0f;
    }
    else
    {
        g_virtW = 640;
        float sx = W / 640.0f, sy = H / 480.0f;
        g_scale = (sx < sy) ? sx : sy;
        g_offX = (W - 640.0f * g_scale) * 0.5f;
        g_offY = (H - 480.0f * g_scale) * 0.5f;
    }
    // where a 4:3 image goes when it must keep its shape (mode 1 menus / loading screens)
    float s43 = (W / 640.0f < H / 480.0f) ? (W / 640.0f) : (H / 480.0f);
    g_off43    = (W - 640.0f * s43) * 0.5f;
    g_offY43   = (H - 480.0f * s43) * 0.5f;
    g_scale43x = (640.0f * s43) / (float)g_virtW;
    g_scale43y = s43;
    g_clearBars = (g_mode == 2) || (g_mode == 1 && g_keep43);
}

static void LoadConfig()
{
    char iniPath[MAX_PATH];
    GetModuleFileNameA(nullptr, iniPath, MAX_PATH); // fall back to exe dir
    // Try DLL's own directory first via a fixed relative name; simplest
    // reliable approach is to just use the current working directory,
    // which for this game is the install folder it was launched from.
    const char* INI_NAME = "PPWidescreenFix.ini";

    g_targetWidth   = GetPrivateProfileIntA("Settings", "Width", 1920, INI_NAME);
    g_targetHeight  = GetPrivateProfileIntA("Settings", "Height", 1080, INI_NAME);
    g_forceViewport = GetPrivateProfileIntA("Settings", "ForceFullViewport", 0, INI_NAME) != 0;
    g_logResources  = GetPrivateProfileIntA("Settings", "LogResourceCalls", 0, INI_NAME) != 0;
    g_recon         = GetPrivateProfileIntA("Settings", "Recon", 1, INI_NAME) != 0;
    g_mode          = GetPrivateProfileIntA("Settings", "Mode", 2, INI_NAME);
    g_keep43        = GetPrivateProfileIntA("Settings", "Keep43Backdrops", 1, INI_NAME) != 0;
    if (g_mode < 0 || g_mode > 2) g_mode = 2;
    ComputeVirtualCanvas();

    // If the ini doesn't exist yet, write out defaults so the user has
    // something to edit next time.
    if (GetFileAttributesA(INI_NAME) == INVALID_FILE_ATTRIBUTES)
    {
        WritePrivateProfileStringA("Settings", "Width", "1920", INI_NAME);
        WritePrivateProfileStringA("Settings", "Height", "1080", INI_NAME);
        WritePrivateProfileStringA("Settings", "ForceFullViewport", "0", INI_NAME);
        WritePrivateProfileStringA("Settings", "LogResourceCalls", "0", INI_NAME);
        WritePrivateProfileStringA("Settings", "Recon", "1", INI_NAME);
        WritePrivateProfileStringA("Settings", "Mode", "2", INI_NAME);
        WritePrivateProfileStringA("Settings", "Keep43Backdrops", "1", INI_NAME);
    }
}

// ---------------------------------------------------------------------------
// dinput8.dll proxy plumbing
// ---------------------------------------------------------------------------
// The exe's only DINPUT8.dll import is DirectInput8Create, so we only need
// to forward that one function. We load the *real* dinput8.dll (renamed to
// dinput8_orig.dll, see README) and pass calls straight through.
typedef HRESULT (WINAPI *DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static DirectInput8Create_t oDirectInput8Create = nullptr;
static HMODULE g_hRealDInput8 = nullptr;

extern "C" __declspec(dllexport)
HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN outer)
{
    if (!oDirectInput8Create)
    {
        if (!g_hRealDInput8)
            g_hRealDInput8 = LoadLibraryA("dinput8_orig.dll");
        if (g_hRealDInput8)
            oDirectInput8Create = (DirectInput8Create_t)GetProcAddress(g_hRealDInput8, "DirectInput8Create");
    }
    if (!oDirectInput8Create)
        return E_FAIL;
    return oDirectInput8Create(hinst, ver, riid, out, outer);
}

// ---------------------------------------------------------------------------
// Hook 1: AdjustWindowRectEx (user32.dll) - the actual resolution root fix
// ---------------------------------------------------------------------------
typedef BOOL (WINAPI *AdjustWindowRectEx_t)(LPRECT, DWORD, BOOL, DWORD);
static AdjustWindowRectEx_t oAdjustWindowRectEx = nullptr;

BOOL WINAPI hkAdjustWindowRectEx(LPRECT lpRect, DWORD dwStyle, BOOL bMenu, DWORD dwExStyle)
{
    if (lpRect)
    {
        int w = lpRect->right - lpRect->left;
        int h = lpRect->bottom - lpRect->top;

        if (w == ORIG_CLIENT_WIDTH && h == ORIG_CLIENT_HEIGHT)
        {
            Log("[AdjustWindowRectEx] rewriting requested client size %dx%d -> %dx%d\n",
                w, h, g_targetWidth, g_targetHeight);
            lpRect->right  = lpRect->left + g_targetWidth;
            lpRect->bottom = lpRect->top  + g_targetHeight;
        }
    }
    return oAdjustWindowRectEx(lpRect, dwStyle, bMenu, dwExStyle);
}

// ---------------------------------------------------------------------------
// Hook 2: AGLSCREEN::SetViewport (Agl7.dll) - force full-frame viewport
// ---------------------------------------------------------------------------
// long AGLSCREEN::SetViewport(DWORD x, DWORD y, DWORD w, DWORD h) - thiscall
typedef long (__thiscall *SetViewport_t)(void* pThis, DWORD x, DWORD y, DWORD w, DWORD h);
static SetViewport_t oSetViewport = nullptr;

long __fastcall hkSetViewport(void* pThis, void* /*edx*/, DWORD x, DWORD y, DWORD w, DWORD h)
{
    long r;
    if (g_mode != 0)
    {
        // Agl7 keeps seeing the VIRTUAL viewport; the real Direct3D viewport is forced
        // to the full surface in hkD3DSetViewport.
        static int n = 0;
        if (n++ < 5)
            Log("[SetViewport] game requested (%u,%u,%u,%u) -> virtual (0,0,%d,%d)\n", x, y, w, h, g_virtW, g_virtH);
        r = oSetViewport(pThis, 0, 0, (DWORD)g_virtW, (DWORD)g_virtH);
    }
    else if (g_forceViewport)
    {
        Log("[SetViewport] game requested (%u,%u,%u,%u) -> forcing (0,0,%d,%d)\n",
            x, y, w, h, g_targetWidth, g_targetHeight);
        r = oSetViewport(pThis, 0, 0, (DWORD)g_targetWidth, (DWORD)g_targetHeight);
    }
    else
    {
        r = oSetViewport(pThis, x, y, w, h);
    }
    TryHookDevice(pThis);
    ApplyVirtualScreen();
    return r;
}

// ---------------------------------------------------------------------------
// Hook 3: AGLSCREEN::SetDisplayMode (Agl7.dll) - diagnostic only
// ---------------------------------------------------------------------------
// We don't need to rewrite this call's parameters (it already reads the
// real client rect after our AdjustWindowRectEx hook has run), but hooking
// it here is useful confirmation while testing, and a safe place to fall
// back to a hard override if a future patch/version reintroduces a
// hardcoded literal here instead of reading GetClientRect.
typedef long (__thiscall *SetDisplayMode_t)(void* pThis, DWORD width, DWORD height, BYTE bpp, BYTE flag);
static SetDisplayMode_t oSetDisplayMode = nullptr;

long __fastcall hkSetDisplayMode(void* pThis, void* /*edx*/, DWORD width, DWORD height, BYTE bpp, BYTE flag)
{
    Log("[SetDisplayMode] width=%u height=%u bpp=%u flag=%u\n", width, height, bpp, flag);

    // Safety net: if for some reason the game still asks for the original
    // 640x480 here (e.g. our AdjustWindowRectEx hook didn't catch the
    // window this ran for), force it anyway.
    if (width == ORIG_CLIENT_WIDTH && height == ORIG_CLIENT_HEIGHT)
    {
        Log("[SetDisplayMode] fallback override triggered\n");
        width  = (DWORD)g_targetWidth;
        height = (DWORD)g_targetHeight;
    }

    long result = oSetDisplayMode(pThis, width, height, bpp, flag);
    Log("[SetDisplayMode] result = %ld\n", result);
    TryHookDevice(pThis);
    return result;
}

// ---------------------------------------------------------------------------
// Hook 4: AGLSCREEN::CreateImage (Agl7.dll) - resource allocation diagnostics
// ---------------------------------------------------------------------------
// This does NOT change any texture/surface size - the 640x256-style
// literals we found elsewhere in the exe look like texture-page atlas
// dimensions, unrelated to screen resolution, and resizing them would
// most likely break sprite lookup math rather than fix anything. This
// hook only logs calls and failures, so that if you hit the
// "missing textures / broken loading screens" symptom from the original
// bug report, the log will show exactly which CreateImage call failed
// (returned null) and at what size, which tells us whether it's a video
// memory exhaustion problem tied to the larger backbuffer.
typedef void* (__thiscall *CreateImage_t)(void* pThis, DWORD w, DWORD h, BYTE type, WORD flags);
static CreateImage_t oCreateImage = nullptr;

typedef long (WINAPI *AGLGetLastError_t)();
static AGLGetLastError_t oAGLGetLastError = nullptr;

void* __fastcall hkCreateImage(void* pThis, void* /*edx*/, DWORD w, DWORD h, BYTE type, WORD flags)
{
    void* result = oCreateImage(pThis, w, h, type, flags);
    TryHookDevice(pThis);
    ApplyVirtualScreen();
    if (g_logResources)
    {
        if (!result && oAGLGetLastError)
        {
            long err = oAGLGetLastError();
            Log("[CreateImage] FAILED w=%u h=%u type=%u flags=%u AGLGetLastError=%ld\n",
                w, h, type, flags, err);
        }
        else
        {
            Log("[CreateImage] ok w=%u h=%u type=%u flags=%u -> %p\n", w, h, type, flags, result);
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// RECON: Direct3D 7 device hooks (v1.2) - LOGGING ONLY, no behaviour change
// ---------------------------------------------------------------------------
// FINDINGS THAT MOTIVATE THIS (verified in the binaries):
//  * Agl7.dll creates a Direct3D 7 HAL device on the back buffer
//    (IDirect3D7::CreateDevice with IID_IDirect3DHALDevice) and stores the
//    IDirect3DDevice7* at AGLSCREEN+0x160.
//  * The exe renders through that device: BeginScene/EndScene around a
//    display-list loop, and 12 DrawPrimitive call sites whose FVF is 0x44 or
//    0x144 = XYZRHW|DIFFUSE(|TEX1). XYZRHW = pre-transformed vertices in
//    SCREEN PIXEL coordinates, computed by the exe. That covers the HUD, text,
//    menu backdrops and the "sky" rectangle.
// So the right place to fix HUD/menu placement is IDirect3DDevice7::DrawPrimitive.
// Before writing remap rules we need to SEE what pixel ranges are used at the
// new resolution - this build only logs that. Send PPWidescreenFix.log back.
//
// IDirect3DDevice7 vtable slots used: 11 SetTransform, 13 SetViewport,
// 25 DrawPrimitive (slot 25 = offset 0x64, matching the exe's call sites).
struct D3DVIEWPORT7_ { DWORD x, y, w, h; float minZ, maxZ; };

typedef HRESULT (WINAPI *DrawPrimitive_t)(void* dev, DWORD type, DWORD fvf, void* verts, DWORD count, DWORD flags);
typedef HRESULT (WINAPI *D3DSetViewport_t)(void* dev, D3DVIEWPORT7_* vp);
typedef HRESULT (WINAPI *D3DSetTransform_t)(void* dev, DWORD state, const float* m);
static DrawPrimitive_t    oDrawPrimitive    = nullptr;
static D3DSetViewport_t   oD3DSetViewport   = nullptr;
static D3DSetTransform_t  oD3DSetTransform  = nullptr;

static void* g_hookedDrawFn = nullptr;     // DrawPrimitive implementation we already hooked
static int   g_budgetQuad   = 1200;        // distinct quad shapes to log   (count==4: sprites / HUD / rectangles)
static int   g_budgetTri    = 250;         // distinct triangle shapes to log (count==3: usually the 3D scene)
static int   g_budgetOther  = 100;         // distinct non-pre-transformed shapes
static int   g_summaryBudget = 60;         // per-interval summary lines
static unsigned int g_cntTri = 0, g_cntQuad = 0, g_cntOther = 0;
static float g_triBox[4]  = { 1e9f, -1e9f, 1e9f, -1e9f };   // minx maxx miny maxy
static float g_quadBox[4] = { 1e9f, -1e9f, 1e9f, -1e9f };

// tiny "have I logged this exact shape already?" set (open addressing, 0 = empty)
static unsigned int g_sigTable[4096];
static bool SeenBefore(unsigned int h)
{
    if (h == 0) h = 1;
    unsigned int idx = h & 4095;
    for (int i = 0; i < 4096; i++)
    {
        unsigned int j = (idx + i) & 4095;
        if (g_sigTable[j] == h) return true;
        if (g_sigTable[j] == 0) { g_sigTable[j] = h; return false; }
    }
    return true; // table full: stop logging new shapes
}
static unsigned int Mix(unsigned int h, unsigned int v) { return (h ^ v) * 16777619u; }

static DWORD FvfStride(DWORD fvf)
{
    DWORD s = 0;
    DWORD pos = fvf & 0x00E;                       // D3DFVF_POSITION_MASK
    if (pos == 0x004)      s += 16;                // XYZRHW
    else if (pos == 0x002) s += 12;                // XYZ
    else return 0;                                 // blend-weight formats: not handled
    if (fvf & 0x010) s += 12;                      // normal
    if (fvf & 0x020) s += 4;                       // reserved / point size
    if (fvf & 0x040) s += 4;                       // diffuse
    if (fvf & 0x080) s += 4;                       // specular
    s += ((fvf >> 8) & 0xF) * 8;                   // texture coordinate sets (2 floats each)
    return s;
}

// Separate function so the SEH guard has no C++ objects to unwind.
static void ReconDraw(DWORD type, DWORD fvf, void* verts, DWORD count)
{
    __try
    {
        if (!verts || count == 0 || count > 64) return;
        DWORD stride = FvfStride(fvf);
        bool tl = ((fvf & 0x00E) == 0x004) && stride >= 16;   // XYZRHW = pixel-space vertices

        if (!tl)
        {
            g_cntOther++;
            if (g_budgetOther > 0)
            {
                unsigned int h = Mix(Mix(Mix(2166136261u, type), fvf), count);
                if (!SeenBefore(h))
                {
                    g_budgetOther--;
                    Log("[DP] prim=%u fvf=0x%X n=%u  (not pre-transformed)\n", type, fvf, count);
                }
            }
            return;
        }

        float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f, rhw = 0.0f, z = 0.0f;
        const BYTE* p = (const BYTE*)verts;
        for (DWORD i = 0; i < count; i++)
        {
            const float* v = (const float*)(p + i * stride);
            if (v[0] < minx) minx = v[0];
            if (v[0] > maxx) maxx = v[0];
            if (v[1] < miny) miny = v[1];
            if (v[1] > maxy) maxy = v[1];
            z = v[2]; rhw = v[3];
        }

        bool quad = (count == 4);
        float* box = quad ? g_quadBox : g_triBox;
        if (quad) g_cntQuad++; else g_cntTri++;
        if (minx < box[0]) box[0] = minx;
        if (maxx > box[1]) box[1] = maxx;
        if (miny < box[2]) box[2] = miny;
        if (maxy > box[3]) box[3] = maxy;

        int* budget = quad ? &g_budgetQuad : &g_budgetTri;
        if (*budget <= 0) return;
        float q = quad ? 16.0f : 64.0f;          // coarser grid for triangles so they don't flood the log
        unsigned int h = 2166136261u;
        h = Mix(h, type); h = Mix(h, fvf); h = Mix(h, count);
        h = Mix(h, (unsigned int)(int)(minx / q)); h = Mix(h, (unsigned int)(int)(maxx / q));
        h = Mix(h, (unsigned int)(int)(miny / q)); h = Mix(h, (unsigned int)(int)(maxy / q));
        if (SeenBefore(h)) return;
        (*budget)--;
        Log("[DP] prim=%u fvf=0x%X n=%u  x[%.1f .. %.1f]  y[%.1f .. %.1f]  z=%.3f rhw=%.3f\n",
            type, fvf, count, minx, maxx, miny, maxy, z, rhw);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// Every 120 frames: how many draws of each kind, and the overall pixel extents of
// 2D quads vs triangles. Tells us the coordinate range the game really uses.
static void ReconFrameSummary()
{
    static int frames = 0;
    if (!g_recon) return;
    if (++frames % 120 != 0 || g_summaryBudget <= 0) return;
    g_summaryBudget--;
    Log("[FRAMES %d] tri=%u quad=%u other=%u | tri x[%.0f..%.0f] y[%.0f..%.0f] | quad x[%.0f..%.0f] y[%.0f..%.0f]\n",
        frames, g_cntTri, g_cntQuad, g_cntOther,
        g_triBox[0], g_triBox[1], g_triBox[2], g_triBox[3],
        g_quadBox[0], g_quadBox[1], g_quadBox[2], g_quadBox[3]);
    g_cntTri = g_cntQuad = g_cntOther = 0;
    g_triBox[0] = g_quadBox[0] = 1e9f;  g_triBox[1] = g_quadBox[1] = -1e9f;
    g_triBox[2] = g_quadBox[2] = 1e9f;  g_triBox[3] = g_quadBox[3] = -1e9f;
}

// Scale pre-transformed (XYZRHW) vertices from the game's virtual canvas to the real surface.
static void RemapVertices(BYTE* buf, DWORD count, DWORD stride, DWORD fvf)
{
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    DWORD i;
    for (i = 0; i < count; i++)
    {
        const float* v = (const float*)(buf + i * stride);
        if (v[0] < minx) minx = v[0];
        if (v[0] > maxx) maxx = v[0];
        if (v[1] < miny) miny = v[1];
        if (v[1] > maxy) maxy = v[1];
    }
    // mode 1: a textured quad covering the whole virtual screen is a 4:3 backdrop
    // (menu / loading / title art) - keep its shape instead of stretching it.
    bool backdrop = (g_mode == 1) && g_keep43 && (count == 4) && ((fvf & 0x100) != 0) &&
                    minx <= 3.0f && miny <= 3.0f &&
                    maxx >= (float)g_virtW - 3.0f && maxy >= (float)g_virtH - 3.0f;
    for (i = 0; i < count; i++)
    {
        float* v = (float*)(buf + i * stride);
        if (backdrop) { v[0] = g_off43 + v[0] * g_scale43x; v[1] = g_offY43 + v[1] * g_scale43y; }
        else          { v[0] = g_offX  + v[0] * g_scale;    v[1] = g_offY  + v[1] * g_scale; }
    }
}

static HRESULT WINAPI hkDrawPrimitive(void* dev, DWORD type, DWORD fvf, void* verts, DWORD count, DWORD flags)
{
    if (g_recon) ReconDraw(type, fvf, verts, count);
    if (g_mode != 0 && verts && count != 0 && count <= 64 && (fvf & 0x00E) == 0x004)
    {
        DWORD stride = FvfStride(fvf);
        if (stride >= 16 && stride <= 80)
        {
            BYTE buf[64 * 80];                       // work on a copy: the game may reuse its buffer
            memcpy(buf, verts, count * stride);
            RemapVertices(buf, count, stride, fvf);
            return oDrawPrimitive(dev, type, fvf, buf, count, flags);
        }
    }
    return oDrawPrimitive(dev, type, fvf, verts, count, flags);
}

static HRESULT WINAPI hkD3DSetViewport(void* dev, D3DVIEWPORT7_* vp)
{
    if (g_recon && vp)
    {
        static D3DVIEWPORT7_ last = { 0xFFFFFFFF, 0, 0, 0, 0, 0 };
        if (memcmp(&last, vp, sizeof(last)) != 0)
        {
            last = *vp;
            Log("[D3D SetViewport] x=%u y=%u w=%u h=%u z=[%.2f .. %.2f]\n", vp->x, vp->y, vp->w, vp->h, vp->minZ, vp->maxZ);
        }
    }
    if (g_mode != 0 && vp)
    {
        D3DVIEWPORT7_ v = *vp;
        v.x = 0; v.y = 0; v.w = (DWORD)g_targetWidth; v.h = (DWORD)g_targetHeight;
        static bool once = false;
        if (!once) { once = true; Log("[D3D SetViewport] virtual mode: forcing the real viewport 0,0,%d,%d\n", g_targetWidth, g_targetHeight); }
        return oD3DSetViewport(dev, &v);
    }
    return oD3DSetViewport(dev, vp);
}

static HRESULT WINAPI hkD3DSetTransform(void* dev, DWORD state, const float* m)
{
    if (g_recon && m && state == 3)   // D3DTRANSFORMSTATE_PROJECTION
    {
        static float last[16];
        static int budget = 60;
        if (budget > 0 && memcmp(last, m, sizeof(last)) != 0)
        {
            memcpy(last, m, sizeof(last));
            budget--;
            Log("[D3D SetTransform PROJECTION] _11=%.4f _22=%.4f _33=%.4f _43=%.4f _34=%.4f\n",
                m[0], m[5], m[10], m[14], m[11]);
        }
    }
    return oD3DSetTransform(dev, state, m);
}

static bool ReadPtrSafe(const void* addr, void** out)
{
    __try { *out = *(void* const*)addr; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Called from every AGLSCREEN hook (they all receive the AGLSCREEN* as 'this').
// Once Agl7 has created the device (AGLSCREEN+0x160) we patch the shared vtable
// functions - only once, since every device uses the same implementations.
static void TryHookDevice(void* screen)
{
    if ((!g_recon && g_mode == 0) || !screen || g_hookedDrawFn) return;

    void* dev = nullptr;
    if (!ReadPtrSafe((const BYTE*)screen + 0x160, &dev) || !dev) return;
    void** vtbl = nullptr;
    if (!ReadPtrSafe(dev, (void**)&vtbl) || !vtbl) return;

    void* fnDraw = nullptr; void* fnVp = nullptr; void* fnTr = nullptr;
    if (!ReadPtrSafe(&vtbl[25], &fnDraw) || !fnDraw) return;
    ReadPtrSafe(&vtbl[13], &fnVp);
    ReadPtrSafe(&vtbl[11], &fnTr);

    Log("[D3D] device found at %p (vtable %p): DrawPrimitive=%p SetViewport=%p SetTransform=%p\n",
        dev, (void*)vtbl, fnDraw, fnVp, fnTr);

    if (MH_CreateHook(fnDraw, (LPVOID)&hkDrawPrimitive, (LPVOID*)&oDrawPrimitive) == MH_OK)
    {
        MH_EnableHook(fnDraw);
        g_hookedDrawFn = fnDraw;
        Log("[D3D] hooked DrawPrimitive\n");
    }
    else Log("[D3D] FAILED to hook DrawPrimitive\n");

    if (fnVp && MH_CreateHook(fnVp, (LPVOID)&hkD3DSetViewport, (LPVOID*)&oD3DSetViewport) == MH_OK)
    { MH_EnableHook(fnVp); Log("[D3D] hooked SetViewport\n"); }
    if (fnTr && MH_CreateHook(fnTr, (LPVOID)&hkD3DSetTransform, (LPVOID*)&oD3DSetTransform) == MH_OK)
    { MH_EnableHook(fnTr); Log("[D3D] hooked SetTransform\n"); }

    // Agl7 already issued its first SetViewport (with the virtual size) before we could hook
    // the device, so set the real full-surface viewport ourselves once.
    if (g_mode != 0 && oD3DSetViewport)
    {
        D3DVIEWPORT7_ v = { 0, 0, (DWORD)g_targetWidth, (DWORD)g_targetHeight, 0.0f, 1.0f };
        HRESULT hr = oD3DSetViewport(dev, &v);
        Log("[D3D] initial real viewport 0,0,%d,%d -> hr=0x%08X\n", g_targetWidth, g_targetHeight, (unsigned)hr);
    }
}

// ---------------------------------------------------------------------------
// VIRTUAL CANVAS helpers (v1.3)
// ---------------------------------------------------------------------------
// exe global at VA 0x443d44 (RVA 0x43d44) points to a context struct whose +8/+0xC
// are the screen width/height used by all layout code (77 read sites). Tied to this
// exact exe build; we sanity-check the values before writing.
static bool PokeCtxDims(void* ctx, DWORD w, DWORD h, DWORD* oldW, DWORD* oldH)
{
    __try
    {
        DWORD* p = (DWORD*)((BYTE*)ctx + 8);
        *oldW = p[0]; *oldH = p[1];
        if (p[0] == w && p[1] == h) return true;
        if (p[0] < 320 || p[0] > 8192 || p[1] < 240 || p[1] > 8192) return false;   // not what we expect
        p[0] = w; p[1] = h;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void ApplyVirtualScreen()
{
    if (g_mode == 0) return;
    BYTE* exeBase = (BYTE*)GetModuleHandleA(NULL);
    void* ctx = nullptr;
    if (!exeBase || !ReadPtrSafe(exeBase + 0x43d44, &ctx) || !ctx) return;     // not created yet
    DWORD ow = 0, oh = 0;
    bool ok = PokeCtxDims(ctx, (DWORD)g_virtW, (DWORD)g_virtH, &ow, &oh);
    static bool logged = false;
    static int  fails = 0;
    if (ok && !logged && (ow != (DWORD)g_virtW || oh != (DWORD)g_virtH))
    {
        logged = true;
        Log("[Virtual] screen context %p: %ux%u -> %dx%d (mode %d, scale %.4f, offset %.1f,%.1f)\n",
            ctx, ow, oh, g_virtW, g_virtH, g_mode, g_scale, g_offX, g_offY);
    }
    else if (!ok && !(ow == 0 && oh == 0) && fails++ < 3)      // 0x0 = exe has not stored the size yet
        Log("[Virtual] could not patch screen context %p (dims %ux%u) - unexpected values, skipped\n", ctx, ow, oh);
}

typedef HRESULT (WINAPI *D3DClear_t)(void* dev, DWORD cnt, void* rects, DWORD flags, DWORD color, float z, DWORD stencil);

// Black out the back buffer after each Flip so the bars never show stale pixels.
static void ClearBackBuffer(void* screen)
{
    if (!g_clearBars || !screen) return;
    void* dev = nullptr; void** vtbl = nullptr; void* fn = nullptr;
    if (!ReadPtrSafe((const BYTE*)screen + 0x160, &dev) || !dev) return;
    if (!ReadPtrSafe(dev, (void**)&vtbl) || !vtbl) return;
    if (!ReadPtrSafe(&vtbl[10], &fn) || !fn) return;                            // slot 10 = Clear
    ((D3DClear_t)fn)(dev, 0, NULL, 1 /*D3DCLEAR_TARGET*/, 0xFF000000, 1.0f, 0);
}

// AGLSCREEN::Flip (called once per frame) - a reliable place to discover the device.
typedef long (__thiscall *Flip_t)(void* pThis, long a, long b);
static Flip_t oFlip = nullptr;
long __fastcall hkFlip(void* pThis, void* /*edx*/, long a, long b)
{
    ReconFrameSummary();
    TryHookDevice(pThis);
    ApplyVirtualScreen();
    long r = oFlip(pThis, a, b);
    ClearBackBuffer(pThis);
    return r;
}

// ---------------------------------------------------------------------------
// PATCH: sky/background rectangle entity (exe-internal, not an Agl7.dll
// export - so there is no function to hook here, only bytes to patch)
// ---------------------------------------------------------------------------
// At exe VA 0x420606/0x42060b, the executable's own code pushes hardcoded
// 480/640 literals as width/height arguments into an internal object
// constructor (VA 0x401e50) that builds a colored background rectangle
// entity - sized to the original resolution regardless of what AGLSCREEN
// reports. At larger resolutions this leaves everything outside that
// rectangle as raw, uncleared (black) backbuffer.
//
// There is no exported function to hook here - this logic lives entirely
// in the exe's own compiled code, not in Agl7.dll - so the only option is
// to patch the two 4-byte immediate operands directly in memory, the same
// two bytes you'd change with a hex editor on disk, just done at runtime
// instead. We verify the original bytes match exactly what we expect
// before writing anything, so that running this against a different
// exe build/version/language either does nothing (safe) rather than
// silently corrupting unrelated code.
//
// IMPORTANT CAVEAT, unlike every hook above: this patch is tied to the
// *exact* v1.0 ENGLISH No-CD/Fixed exe build by raw address. The
// AdjustWindowRectEx/SetViewport/SetDisplayMode/CreateImage hooks above
// all target Agl7.dll's exported function *names*, so they keep working
// even if a different exe build changes its own internal code layout.
// This patch does not have that protection - it is inherently more
// fragile, which is exactly why the verify-before-write check matters.
static bool PatchImmediateIfMatches(BYTE* base, DWORD rva, DWORD expectedOldValue, DWORD newValue, const char* label)
{
    BYTE* addr = base + rva;
    DWORD currentValue;
    memcpy(&currentValue, addr, sizeof(DWORD));

    if (currentValue != expectedOldValue)
    {
        Log("[Patch] SKIPPED %s at RVA 0x%X - expected 0x%X, found 0x%X "
            "(different exe build/version? patch not applied, nothing touched)\n",
            label, rva, expectedOldValue, currentValue);
        return false;
    }

    DWORD oldProtect;
    if (!VirtualProtect(addr, sizeof(DWORD), PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        Log("[Patch] FAILED to unprotect memory for %s\n", label);
        return false;
    }
    memcpy(addr, &newValue, sizeof(DWORD));
    VirtualProtect(addr, sizeof(DWORD), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), addr, sizeof(DWORD));

    Log("[Patch] %s: 0x%X -> 0x%X at RVA 0x%X\n", label, expectedOldValue, newValue, rva);
    return true;
}

static void PatchSkyRectangleEntity()
{
    BYTE* exeBase = (BYTE*)GetModuleHandleA(NULL);
    if (!exeBase)
    {
        Log("[Patch] Could not get exe base address - skipping sky-rect patch\n");
        return;
    }

    // RVA 0x20606 = "68 e0 01 00 00" (push 0x1e0 / 480) - the immediate
    // operand is the 4 bytes AFTER the 0x68 push opcode, i.e. RVA+1.
    PatchImmediateIfMatches(exeBase, 0x20607, (DWORD)ORIG_CLIENT_HEIGHT,
                             (DWORD)(g_mode != 0 ? g_virtH : g_targetHeight), "sky-rect height");

    // RVA 0x2060b = "68 80 02 00 00" (push 0x280 / 640), same pattern.
    PatchImmediateIfMatches(exeBase, 0x2060c, (DWORD)ORIG_CLIENT_WIDTH,
                             (DWORD)(g_mode != 0 ? g_virtW : g_targetWidth), "sky-rect width");
}

// ---------------------------------------------------------------------------
// Hook installation
// ---------------------------------------------------------------------------
static void InstallHooks()
{
    if (MH_Initialize() != MH_OK)
    {
        Log("MH_Initialize failed\n");
        return;
    }

    // --- AdjustWindowRectEx (user32.dll) ---
    HMODULE hUser32 = GetModuleHandleA("user32.dll");
    if (hUser32)
    {
        void* target = GetProcAddress(hUser32, "AdjustWindowRectEx");
        if (target && MH_CreateHook(target, &hkAdjustWindowRectEx, (void**)&oAdjustWindowRectEx) == MH_OK)
        {
            MH_EnableHook(target);
            Log("Hooked AdjustWindowRectEx\n");
        }
        else
        {
            Log("FAILED to hook AdjustWindowRectEx\n");
        }
    }

    // --- Agl7.dll exports ---
    HMODULE hAgl = GetModuleHandleA("Agl7.dll");
    if (!hAgl) hAgl = LoadLibraryA("Agl7.dll");

    if (hAgl)
    {
        void* pSetViewport = GetProcAddress(hAgl, "?SetViewport@AGLSCREEN@@QAEJKKKK@Z");
        if (pSetViewport && MH_CreateHook(pSetViewport, &hkSetViewport, (void**)&oSetViewport) == MH_OK)
        {
            MH_EnableHook(pSetViewport);
            Log("Hooked AGLSCREEN::SetViewport\n");
        }
        else
        {
            Log("FAILED to hook AGLSCREEN::SetViewport (export not found?)\n");
        }

        void* pSetDisplayMode = GetProcAddress(hAgl, "?SetDisplayMode@AGLSCREEN@@QAEJKKEE@Z");
        if (pSetDisplayMode && MH_CreateHook(pSetDisplayMode, &hkSetDisplayMode, (void**)&oSetDisplayMode) == MH_OK)
        {
            MH_EnableHook(pSetDisplayMode);
            Log("Hooked AGLSCREEN::SetDisplayMode\n");
        }
        else
        {
            Log("FAILED to hook AGLSCREEN::SetDisplayMode (export not found?)\n");
        }

        void* pCreateImage = GetProcAddress(hAgl, "?CreateImage@AGLSCREEN@@QAEPAVAGLIMAGE@@KKEG@Z");
        if (pCreateImage && MH_CreateHook(pCreateImage, &hkCreateImage, (void**)&oCreateImage) == MH_OK)
        {
            MH_EnableHook(pCreateImage);
            Log("Hooked AGLSCREEN::CreateImage\n");
        }
        else
        {
            Log("FAILED to hook AGLSCREEN::CreateImage (export not found?)\n");
        }

        void* pFlip = GetProcAddress(hAgl, "?Flip@AGLSCREEN@@QAEJJJ@Z");
        if (pFlip && MH_CreateHook(pFlip, (LPVOID)&hkFlip, (void**)&oFlip) == MH_OK)
        {
            MH_EnableHook(pFlip);
            Log("Hooked AGLSCREEN::Flip (device discovery)\n");
        }
        else
        {
            Log("FAILED to hook AGLSCREEN::Flip\n");
        }

        oAGLGetLastError = (AGLGetLastError_t)GetProcAddress(hAgl, "?AGLGetLastError@@YAJXZ");
    }
    else
    {
        Log("Agl7.dll not found/loadable - is this DLL sitting in the game folder?\n");
    }

    // --- exe-internal sky/background rectangle patch (see comment above) ---
    PatchSkyRectangleEntity();
}

// ---------------------------------------------------------------------------
// DllMain
// ---------------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        LogInit();
        LoadConfig();
        Log("PPWidescreenFix v1.3 loaded. Target resolution: %dx%d\n", g_targetWidth, g_targetHeight);
        Log("[Mode] %d  virtual canvas %dx%d  scale=%.4f  offset=(%.1f,%.1f)  keep43=%d  clearBars=%d\n",
            g_mode, g_virtW, g_virtH, g_scale, g_offX, g_offY, (int)g_keep43, (int)g_clearBars);
        InstallHooks();
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        MH_Uninitialize();
        if (g_log) fclose(g_log);
    }
    return TRUE;
}
