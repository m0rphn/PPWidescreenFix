// ============================================================================
// PPWidescreenFix v1.5 - native resolution / widescreen plugin for
//   Pink Panther: Pinkadelic Pursuit (2002)  -  v1.0 ENGLISH No-CD/Fixed EXE
//
// PROJECT GOAL
// -----------------------------------------------------------------------------
// This is a clean native fix from first principles: no DDrawCompat wrapper, no
// dgVoodoo2-style compatibility layer, and no reliance on winmm/ogg-winmm as a
// prerequisite for the graphical path. The plugin is a dinput8 proxy that fixes
// the engine's own 640x480 layout at runtime and remaps it to the real monitor
// resolution.
//
// HOW THE GAME RENDERS (verified from the binaries and from the v1.2/v1.3 logs)
// -----------------------------------------------------------------------------
//  * Agl7.dll creates a Direct3D 7 HAL device on the DirectDraw back buffer and
//    stores the IDirect3DDevice7* at AGLSCREEN+0x160.
//  * The exe draws EVERYTHING with IDirect3DDevice7::DrawPrimitive using
//    pre-transformed XYZRHW vertices (FVF 0x44 / 0x144): HUD, text, menu art and
//    the 3D world, which the game projects in software (focal length is the
//    constant 479.0, centre = viewport size / 2, so a wider canvas = Hor+).
//  * Layout code reads the screen size from a context struct (exe VA 0x443d44,
//    +8 width, +0xC height): text is centred on width/2, some HUD items are
//    anchored to width - N, others sit at fixed positions on the left.
//  * The game's own D3D viewport is ALWAYS the full context rectangle, and the
//    game does not clip polygons itself: polygons that stick out of the screen
//    (x from about -580 to +1200 ...) were cut off by the D3D viewport.
//
// WHAT THE PLUGIN DOES
// -----------------------------------------------------------------------------
//  1. AdjustWindowRectEx hook: the 640x480 client rect becomes the real size.
//  2. A VIRTUAL CANVAS: the exe lays everything out on a virtual size stored in
//     its context struct; every pre-transformed vertex is scaled to the real
//     surface inside the DrawPrimitive hook, so the whole game stays consistent.
//        Mode 1 = (default) virtual canvas with the aspect ratio of the screen
//                 (853x480 at 16:9) that fills the screen: TRUE WIDESCREEN (Hor+),
//                 no black bars. Full-screen 4:3 pictures (loading / title / menu
//                 art) are stretched to the screen (BackdropFit=1) or keep their
//                 4:3 shape with black bars (BackdropFit=0).
//        Mode 2 = virtual 640x480 canvas, centred with black bars (pillarbox) -
//                 the original game, useful as a reference.
//        Mode 0 = off (resolution patch only)
//  3. CLIPPING: every pre-transformed triangle/quad is clipped to the virtual
//     canvas on the CPU before it is scaled (perspective-correct texture
//     coordinates, linear colour) - this replaces the clipping that the original
//     640x480 D3D viewport used to do. (v1.3 forced the viewport to the whole
//     surface without clipping, so off-screen geometry showed up in the bars.)
//  4. The virtual size is applied EARLY: right after SetDisplayMode the exe stores
//     the REAL size in its context struct (VA 0x402c2f/0x402c37) and the next
//     Direct3D call it makes is IDirect3DDevice7::SetTextureStageState (VA
//     0x402cf1); the plugin hooks that call and overwrites the struct right there,
//     before any entity can capture the real size. A safety net in the draw hook
//     also normalises any full-screen quad that still has real-size coordinates.
//
// NOTE: several patches/addresses are tied to this exact exe build; they are
// verified before anything is written.
// ============================================================================

#define NOMINMAX
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include "MinHook.h"

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
static int  g_targetWidth   = 1920;
static int  g_targetHeight  = 1080;
static bool g_forceViewport = true;
static bool g_logResources  = false;
static bool g_logToFile     = true;
static bool g_recon         = false;  // enable detailed Direct3D diagnostics only when needed

//   Mode 0 = off, Mode 1 = widescreen (Hor+), Mode 2 = 640x480 pillarbox
static int   g_mode          = 1;
static int   g_backdropFit   = 1;      // mode 1, full-screen 4:3 pictures: 0 = keep 4:3 (black bars), 1 = stretch to the screen
static bool  g_clipToCanvas  = true;   // clip all pre-transformed primitives to the virtual canvas
static int   g_virtW         = 640, g_virtH = 480;
static float g_scaleX        = 1.0f, g_scaleY = 1.0f, g_offX = 0.0f, g_offY = 0.0f;   // virtual canvas -> real surface
static float g_off43         = 0.0f, g_offY43 = 0.0f, g_scale43x = 1.0f, g_scale43y = 1.0f;   // 4:3 placement (BackdropFit=0)
static bool  g_clearBars     = false;

static const int ORIG_CLIENT_WIDTH  = 640;
static const int ORIG_CLIENT_HEIGHT = 480;

static FILE* g_log = nullptr;

// defined further down; the AGLSCREEN hooks call them
static void TryHookDevice(void* screen);
static bool ApplyVirtualScreen(const char* who = "Agl7 hook");
static bool g_sdmReturned = false;   // AGLSCREEN::SetDisplayMode has returned to the exe
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
        // virtual canvas with the aspect ratio of the screen; it fills the screen exactly
        // (the two scale factors differ by well under 0.1%, which is invisible)
        g_virtW = (int)(480.0 * g_targetWidth / g_targetHeight + 0.5);
        if (g_virtW < 640) g_virtW = 640;
        g_scaleX = W / (float)g_virtW;
        g_scaleY = H / 480.0f;
        g_offX = 0.0f;
        g_offY = 0.0f;
    }
    else
    {
        g_virtW = 640;
        float sx = W / 640.0f, sy = H / 480.0f;
        g_scaleX = g_scaleY = (sx < sy) ? sx : sy;
        g_offX = (W - 640.0f * g_scaleX) * 0.5f;
        g_offY = (H - 480.0f * g_scaleY) * 0.5f;
    }
    // where a full-screen 4:3 picture goes when it must keep its shape (mode 1, BackdropFit=0)
    float s43 = (W / 640.0f < H / 480.0f) ? (W / 640.0f) : (H / 480.0f);
    g_off43    = (W - 640.0f * s43) * 0.5f;
    g_offY43   = (H - 480.0f * s43) * 0.5f;
    g_scale43x = (640.0f * s43) / (float)g_virtW;
    g_scale43y = s43;
    g_clearBars = (g_mode == 2) || (g_mode == 1 && g_backdropFit == 0);
}

static void LoadConfig()
{
    const char* INI_NAME = "PPWidescreenFix.ini";   // current directory = the game folder

    g_targetWidth   = GetPrivateProfileIntA("Settings", "Width", 1920, INI_NAME);
    g_targetHeight  = GetPrivateProfileIntA("Settings", "Height", 1080, INI_NAME);
    g_forceViewport = GetPrivateProfileIntA("Settings", "ForceFullViewport", 0, INI_NAME) != 0;
    g_logResources  = GetPrivateProfileIntA("Settings", "LogResourceCalls", 0, INI_NAME) != 0;
    g_recon         = GetPrivateProfileIntA("Settings", "Recon", 0, INI_NAME) != 0;
    g_mode          = GetPrivateProfileIntA("Settings", "Mode", 1, INI_NAME);
    g_backdropFit   = GetPrivateProfileIntA("Settings", "BackdropFit", 1, INI_NAME);
    g_clipToCanvas  = GetPrivateProfileIntA("Settings", "ClipToCanvas", 1, INI_NAME) != 0;
    if (g_mode < 0 || g_mode > 2) g_mode = 1;
    if (g_backdropFit < 0 || g_backdropFit > 1) g_backdropFit = 1;
    if (g_targetWidth < 640 || g_targetHeight < 480) { g_targetWidth = 1920; g_targetHeight = 1080; }
    ComputeVirtualCanvas();

    // If the ini doesn't exist yet, write out defaults so the user has something to edit.
    if (GetFileAttributesA(INI_NAME) == INVALID_FILE_ATTRIBUTES)
    {
        WritePrivateProfileStringA("Settings", "Width", "1920", INI_NAME);
        WritePrivateProfileStringA("Settings", "Height", "1080", INI_NAME);
        WritePrivateProfileStringA("Settings", "Mode", "1", INI_NAME);
        WritePrivateProfileStringA("Settings", "BackdropFit", "1", INI_NAME);
        WritePrivateProfileStringA("Settings", "ClipToCanvas", "1", INI_NAME);
        WritePrivateProfileStringA("Settings", "ForceFullViewport", "0", INI_NAME);
        WritePrivateProfileStringA("Settings", "LogResourceCalls", "0", INI_NAME);
        WritePrivateProfileStringA("Settings", "Recon", "0", INI_NAME);
    }
}

// ---------------------------------------------------------------------------
// dinput8.dll proxy plumbing
// ---------------------------------------------------------------------------
// The exe's only DINPUT8.dll import is DirectInput8Create. Load the Windows
// copy by absolute system-directory path so no renamed game-folder DLL is needed.
typedef HRESULT (WINAPI *DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static DirectInput8Create_t oDirectInput8Create = nullptr;
static HMODULE g_hRealDInput8 = nullptr;
static INIT_ONCE g_dinput8InitOnce = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK InitializeRealDInput8(PINIT_ONCE, PVOID, PVOID*)
{
    wchar_t path[MAX_PATH];
    UINT length = GetSystemDirectoryW(path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
    {
        if (length >= MAX_PATH) SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    const wchar_t dllName[] = L"\\dinput8.dll";
    const size_t dllNameLength = sizeof(dllName) / sizeof(dllName[0]);
    if ((size_t)length + dllNameLength > MAX_PATH)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memcpy(path + length, dllName, sizeof(dllName));

    HMODULE module = LoadLibraryW(path);
    if (!module) return FALSE;

    DirectInput8Create_t proc =
        (DirectInput8Create_t)GetProcAddress(module, "DirectInput8Create");
    if (!proc)
    {
        DWORD error = GetLastError();
        FreeLibrary(module);
        SetLastError(error);
        return FALSE;
    }

    g_hRealDInput8 = module;
    oDirectInput8Create = proc;
    return TRUE;
}

extern "C"
HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN outer)
{
    if (!InitOnceExecuteOnce(&g_dinput8InitOnce, InitializeRealDInput8, nullptr, nullptr))
    {
        DWORD error = GetLastError();
        return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
    }
    return oDirectInput8Create(hinst, ver, riid, out, outer);
}

// ---------------------------------------------------------------------------
// Hook 1: AdjustWindowRectEx (user32.dll) - the resolution root fix
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
// Hook 2: AGLSCREEN::SetViewport (Agl7.dll)
// ---------------------------------------------------------------------------
// long AGLSCREEN::SetViewport(DWORD x, DWORD y, DWORD w, DWORD h) - thiscall.
// Agl7 stores w/h as floats at AGLSCREEN+0x84/+0x88 and the exe derives the 3D
// projection centre (ctx+0x14/+0x18 = w/2, h/2) from them, so Agl7 must keep
// seeing the VIRTUAL size. The real D3D viewport is forced in hkD3DSetViewport.
typedef long (__thiscall *SetViewport_t)(void* pThis, DWORD x, DWORD y, DWORD w, DWORD h);
static SetViewport_t oSetViewport = nullptr;

long __fastcall hkSetViewport(void* pThis, void* /*edx*/, DWORD x, DWORD y, DWORD w, DWORD h)
{
    long r;
    if (g_mode != 0)
    {
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
// Hook 3: AGLSCREEN::SetDisplayMode (Agl7.dll)
// ---------------------------------------------------------------------------
typedef long (__thiscall *SetDisplayMode_t)(void* pThis, DWORD width, DWORD height, BYTE bpp, BYTE flag);
static SetDisplayMode_t oSetDisplayMode = nullptr;

long __fastcall hkSetDisplayMode(void* pThis, void* /*edx*/, DWORD width, DWORD height, BYTE bpp, BYTE flag)
{
    Log("[SetDisplayMode] width=%u height=%u bpp=%u flag=%u\n", width, height, bpp, flag);

    // Safety net: if the game still asks for the original 640x480 here, force the real size.
    if (width == (DWORD)ORIG_CLIENT_WIDTH && height == (DWORD)ORIG_CLIENT_HEIGHT)
    {
        Log("[SetDisplayMode] fallback override triggered\n");
        width  = (DWORD)g_targetWidth;
        height = (DWORD)g_targetHeight;
    }

    long result = oSetDisplayMode(pThis, width, height, bpp, flag);
    Log("[SetDisplayMode] result = %ld\n", result);
    TryHookDevice(pThis);
    g_sdmReturned = true;     // from here on the exe stores the real size in its context struct
    return result;
}

// ---------------------------------------------------------------------------
// Hook 4: AGLSCREEN::CreateImage (Agl7.dll) - resource allocation diagnostics
// ---------------------------------------------------------------------------
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
// Direct3D 7 device hooks
// ---------------------------------------------------------------------------
// IDirect3DDevice7 vtable slots used: 10 Clear, 11 SetTransform, 13 SetViewport,
// 25 DrawPrimitive (offset 0x64, matching the exe's call sites).
struct D3DVIEWPORT7_ { DWORD x, y, w, h; float minZ, maxZ; };

typedef HRESULT (WINAPI *DrawPrimitive_t)(void* dev, DWORD type, DWORD fvf, void* verts, DWORD count, DWORD flags);
typedef HRESULT (WINAPI *D3DSetViewport_t)(void* dev, D3DVIEWPORT7_* vp);
typedef HRESULT (WINAPI *D3DSetTransform_t)(void* dev, DWORD state, const float* m);
static DrawPrimitive_t    oDrawPrimitive    = nullptr;
static D3DSetViewport_t   oD3DSetViewport   = nullptr;
static D3DSetTransform_t  oD3DSetTransform  = nullptr;

static void* g_hookedDrawFn = nullptr;     // DrawPrimitive implementation we already hooked
static int   g_budgetUI     = 500;         // distinct 2D-UI shapes to log  (z == 0)
static int   g_budgetQuad   = 150;         // distinct 3D quad shapes to log
static int   g_budgetTri    = 100;         // distinct triangle shapes to log
static int   g_budgetOther  = 60;          // distinct non-pre-transformed shapes
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
// Logs the game's own (virtual-canvas) coordinates, before any remapping.
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
        bool ui   = (z > -1e-6f && z < 1e-6f);
        float* box = quad ? g_quadBox : g_triBox;
        if (quad) g_cntQuad++; else g_cntTri++;
        if (minx < box[0]) box[0] = minx;
        if (maxx > box[1]) box[1] = maxx;
        if (miny < box[2]) box[2] = miny;
        if (maxy > box[3]) box[3] = maxy;

        int* budget = quad ? (ui ? &g_budgetUI : &g_budgetQuad) : &g_budgetTri;
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
// 2D quads vs triangles (game coordinates, i.e. virtual canvas).
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

// ==== PP_PURE_BEGIN ==========================================================
// Primitive processor: clip to the virtual canvas, then scale to the real surface.
// No Windows / D3D dependencies - it is unit-tested on its own.
typedef unsigned int  PP_U32;
typedef unsigned char PP_U8;

static const int PP_MAX_DW   = 20;      // largest supported vertex = 80 bytes
static const int PP_POLY_MAX = 16;      // working polygon size while clipping one triangle
static const int PP_CLIP_MAXVERTS = 16; // clip primitives with at most this many vertices
static const int PP_OUT_CAP  = 256;     // output vertices per DrawPrimitive call

struct PPVert { PP_U32 d[PP_MAX_DW]; };

static inline float PP_F(const PPVert& v, int i) { float f; memcpy(&f, &v.d[i], 4); return f; }
static inline void  PP_SF(PPVert& v, int i, float f) { memcpy(&v.d[i], &f, 4); }

static bool PP_ValidScreenPosition(float x, float y, float z, float rhw)
{
    // Keep ample room for projected off-screen geometry, but reject values that
    // cannot be meaningful screen positions and would poison CPU clipping.
    const double limit = (double)(g_targetWidth > g_targetHeight ? g_targetWidth : g_targetHeight) * 16.0;
    return x == x && y == y && z == z && rhw == rhw &&
           x >= -limit && x <= limit && y >= -limit && y <= limit &&
           fabs((double)z) <= 1e30 && fabs((double)rhw) <= 1e30;
}

struct PPLayout { int strideDW; int diffuse; int specular; int tex; int nTex; };   // offsets in DWORDs, -1 = absent

struct PPStats { unsigned passed, clipped, culled, realsize, backdrop, invalid; };
static PPStats g_ppStats = { 0, 0, 0, 0, 0, 0 };

static bool PP_MakeLayout(PP_U32 fvf, PPLayout* L)
{
    if ((fvf & 0x00E) != 0x004) return false;       // XYZRHW only
    if (fvf & 0xFFFF0000u) return false;            // texcoord size overrides etc.: not handled
    if (fvf & 0x010) return false;                  // normal: not valid with XYZRHW
    int s = 4;                                      // x y z rhw
    if (fvf & 0x020) s += 1;                        // reserved / point size
    L->diffuse = -1; L->specular = -1;
    if (fvf & 0x040) { L->diffuse  = s; s += 1; }
    if (fvf & 0x080) { L->specular = s; s += 1; }
    L->nTex = (int)((fvf >> 8) & 0xF);
    L->tex  = s; s += L->nTex * 2;
    L->strideDW = s;
    return s <= PP_MAX_DW;
}

static PP_U32 PP_LerpColor(PP_U32 a, PP_U32 b, double t)
{
    PP_U32 o = 0;
    for (int s = 0; s < 32; s += 8)
    {
        double ca = (double)((a >> s) & 0xFF), cb = (double)((b >> s) & 0xFF);
        int v = (int)(ca + (cb - ca) * t + 0.5);
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        o |= (PP_U32)v << s;
    }
    return o;
}

// New vertex at parameter t on the edge a->b (screen space). Position is given
// explicitly (it lies exactly on the clip line). z, rhw and colours are linear in
// screen space; texture coordinates are perspective-correct (u/rhw is linear).
static void PP_Lerp(PPVert& out, const PPVert& a, const PPVert& b, double t, double nx, double ny, const PPLayout& L)
{
    out = a;
    PP_SF(out, 0, (float)nx);
    PP_SF(out, 1, (float)ny);
    double za = PP_F(a, 2), zb = PP_F(b, 2);
    double ra = PP_F(a, 3), rb = PP_F(b, 3);
    double rr = ra + (rb - ra) * t;
    PP_SF(out, 2, (float)(za + (zb - za) * t));
    PP_SF(out, 3, (float)rr);
    if (L.diffuse  >= 0) out.d[L.diffuse]  = PP_LerpColor(a.d[L.diffuse],  b.d[L.diffuse],  t);
    if (L.specular >= 0) out.d[L.specular] = PP_LerpColor(a.d[L.specular], b.d[L.specular], t);
    bool persp = (ra > 1e-9 && rb > 1e-9 && rr > 1e-9);
    for (int k = 0; k < L.nTex * 2; k++)
    {
        double ua = PP_F(a, L.tex + k), ub = PP_F(b, L.tex + k);
        double u;
        if (persp) u = (ua * ra + (ub * rb - ua * ra) * t) / rr;
        else       u = ua + (ub - ua) * t;
        PP_SF(out, L.tex + k, (float)u);
    }
}

// One Sutherland-Hodgman pass. axis 0 = x, 1 = y. keepGE: keep v >= bound, else v <= bound.
static int PP_ClipPass(const PPVert* in, int n, PPVert* out, int axis, bool keepGE, double bound, const PPLayout& L)
{
    int m = 0;
    if (n <= 0) return 0;
    const PPVert* prev = &in[n - 1];
    double pp = PP_F(*prev, axis);
    bool pin = keepGE ? (pp >= bound) : (pp <= bound);
    for (int i = 0; i < n; i++)
    {
        const PPVert* cur = &in[i];
        double pc = PP_F(*cur, axis);
        bool cin = keepGE ? (pc >= bound) : (pc <= bound);
        if (cin != pin)
        {
            double t  = (bound - pp) / (pc - pp);
            double ox = PP_F(*prev, 0), oy = PP_F(*prev, 1);
            double cx = PP_F(*cur, 0),  cy = PP_F(*cur, 1);
            double nx = ox + (cx - ox) * t, ny = oy + (cy - oy) * t;
            if (axis == 0) nx = bound; else ny = bound;
            if (m < PP_POLY_MAX) { PP_Lerp(out[m], *prev, *cur, t, nx, ny, L); m++; }
        }
        if (cin && m < PP_POLY_MAX) out[m++] = *cur;
        prev = cur; pp = pc; pin = cin;
    }
    return m;
}

// Clips one triangle to [x0,x1]x[y0,y1]; the result is fan-triangulated and written
// to 'out' as a triangle list (winding order of the input is preserved). Returns the
// number of vertices written (0 = nothing visible).
static int PP_ClipTri(const PPVert& a, const PPVert& b, const PPVert& c, const PPLayout& L,
                      float x0, float y0, float x1, float y1, PPVert* out, int cap)
{
    PPVert A[PP_POLY_MAX], B[PP_POLY_MAX];
    A[0] = a; A[1] = b; A[2] = c;
    int n = 3;
    n = PP_ClipPass(A, n, B, 0, true,  (double)x0, L); if (n < 3) return 0;
    n = PP_ClipPass(B, n, A, 0, false, (double)x1, L); if (n < 3) return 0;
    n = PP_ClipPass(A, n, B, 1, true,  (double)y0, L); if (n < 3) return 0;
    n = PP_ClipPass(B, n, A, 1, false, (double)y1, L); if (n < 3) return 0;
    for (int i = 0; i < n; i++)             // remove float rounding noise at the border
    {
        float x = PP_F(A[i], 0), y = PP_F(A[i], 1);
        if (x < x0) x = x0;
        if (x > x1) x = x1;
        if (y < y0) y = y0;
        if (y > y1) y = y1;
        PP_SF(A[i], 0, x); PP_SF(A[i], 1, y);
    }
    int w = 0;
    for (int i = 1; i + 1 < n; i++)
    {
        if (w + 3 > cap) break;
        out[w++] = A[0]; out[w++] = A[i]; out[w++] = A[i + 1];
    }
    return w;
}

static PPVert g_ppWork[64];
static PPVert g_ppTmp[PP_OUT_CAP];

enum { PP_MAP_WIDE = 0, PP_MAP_BACKDROP43 = 1 };

// Takes one DrawPrimitive call from the game (virtual-canvas coordinates) and produces
// the call to make on the real device.
//   >0 : draw 'out' with *outType / *outCount   (out holds the same FVF)
//    0 : nothing to draw (outside the canvas / not finite)
//   <0 : not handled - the caller should pass the original call through unchanged
static int PP_Prepare(PP_U32 type, PP_U32 fvf, const void* verts, PP_U32 count,
                      PP_U8* out, PP_U32* outType, PP_U32* outCount)
{
    PPLayout L;
    if (!verts || count == 0 || count > 64 || !PP_MakeLayout(fvf, &L)) return -1;
    const PP_U32 strideB = (PP_U32)L.strideDW * 4u;
    const PP_U8* src = (const PP_U8*)verts;
    PP_U32 i;
    for (i = 0; i < count; i++)
    {
        memset(&g_ppWork[i], 0, sizeof(PPVert));
        memcpy(&g_ppWork[i], src + i * strideB, strideB);
    }

    // bounding box, 2D-class test (z == 0: HUD / menus / fills; the 3D world has z > 0)
    float minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f;
    bool ui = true;
    for (i = 0; i < count; i++)
    {
        float x = PP_F(g_ppWork[i], 0), y = PP_F(g_ppWork[i], 1), z = PP_F(g_ppWork[i], 2);
        float rhw = PP_F(g_ppWork[i], 3);
        if (!PP_ValidScreenPosition(x, y, z, rhw)) { g_ppStats.invalid++; return 0; }
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
        if (z > 1e-6f || z < -1e-6f) ui = false;
    }

    // Safety net: a full-screen 2D quad that still carries REAL-surface coordinates
    // (built before the virtual size was known) -> bring it into the virtual canvas.
    if (count == 4 && ui && (g_targetWidth != g_virtW || g_targetHeight != g_virtH))
    {
        float RW = (float)g_targetWidth, RH = (float)g_targetHeight;
        if (minx >= -8.0f && minx <= 4.0f && miny >= -8.0f && miny <= 4.0f &&
            maxx >= RW - 4.0f && maxx <= RW + 4.0f && maxy >= RH - 4.0f && maxy <= RH + 6.0f)
        {
            float kx = (float)g_virtW / RW, ky = (float)g_virtH / RH;
            for (i = 0; i < count; i++)
            {
                PP_SF(g_ppWork[i], 0, PP_F(g_ppWork[i], 0) * kx);
                PP_SF(g_ppWork[i], 1, PP_F(g_ppWork[i], 1) * ky);
            }
            minx *= kx; maxx *= kx; miny *= ky; maxy *= ky;
            g_ppStats.realsize++;
        }
    }

    // Older intro/backdrop draws can retain the original 640x480 coordinates
    // after the context has been virtualized. Restrict this correction to an
    // exact full-screen textured quad so ordinary 640x480-sized art is untouched.
    bool legacyBackdrop = count == 4 && (type == 5 || type == 6) && ui && L.nTex > 0 &&
                          minx >= -8.0f && minx <= 4.0f &&
                          miny >= -8.0f && miny <= 4.0f &&
                          maxx >= 636.0f && maxx <= 644.0f &&
                          maxy >= 476.0f && maxy <= 486.0f;
    if (legacyBackdrop && g_mode == 1 && g_virtW != 640)
    {
        float kx = (float)g_virtW / 640.0f;
        for (i = 0; i < count; i++)
            PP_SF(g_ppWork[i], 0, PP_F(g_ppWork[i], 0) * kx);
        minx *= kx;
        maxx *= kx;
    }

    // A full-canvas textured 2D quad is a 4:3 picture (loading / title / menu art). With
    // BackdropFit=1 it is simply stretched with everything else; with BackdropFit=0 it keeps
    // its 4:3 shape, centred (the bars are cleared to black every frame).
    bool backdrop = (count == 4) && L.nTex > 0 && ui &&
                    minx <= 3.0f && miny <= 3.0f &&
                    maxx >= (float)g_virtW - 3.0f && maxy >= (float)g_virtH - 3.0f;
    int kind = PP_MAP_WIDE;
    if (backdrop)
    {
        g_ppStats.backdrop++;
        if (g_mode == 1 && g_backdropFit == 0) kind = PP_MAP_BACKDROP43;
    }

    // clip rectangle = the virtual canvas, in game coordinates
    const float cx0 = 0.0f, cy0 = 0.0f, cx1 = (float)g_virtW, cy1 = (float)g_virtH;

    bool inside  = (minx >= cx0 && maxx <= cx1 && miny >= cy0 && maxy <= cy1);
    bool outside = (maxx < cx0 || minx > cx1 || maxy < cy0 || miny > cy1);
    bool doClip  = g_clipToCanvas && !inside;
    if (doClip && outside) { g_ppStats.culled++; return 0; }

    PPVert* res = g_ppWork;
    PP_U32 resType = type, resCount = count;

    if (doClip)
    {
        int tri[3 * 14];
        int nTri = 0;
        if (count <= (PP_U32)PP_CLIP_MAXVERTS)
        {
            if (type == 4 && count >= 3 && count % 3 == 0)
            {
                nTri = (int)(count / 3);
                for (int t = 0; t < nTri; t++) { tri[t*3] = t*3; tri[t*3+1] = t*3+1; tri[t*3+2] = t*3+2; }
            }
            else if (type == 5 && count >= 3)                    // strip: D3D order (v0 v1 v2), (v1 v3 v2), ...
            {
                nTri = (int)count - 2;
                for (int t = 0; t < nTri; t++)
                {
                    if (t & 1) { tri[t*3] = t; tri[t*3+1] = t+2; tri[t*3+2] = t+1; }
                    else       { tri[t*3] = t; tri[t*3+1] = t+1; tri[t*3+2] = t+2; }
                }
            }
            else if (type == 6 && count >= 3)                    // fan
            {
                nTri = (int)count - 2;
                for (int t = 0; t < nTri; t++) { tri[t*3] = 0; tri[t*3+1] = t+1; tri[t*3+2] = t+2; }
            }
        }
        if (nTri > 0)
        {
            int nv = 0;
            for (int t = 0; t < nTri; t++)
                nv += PP_ClipTri(g_ppWork[tri[t*3]], g_ppWork[tri[t*3+1]], g_ppWork[tri[t*3+2]], L,
                                 cx0, cy0, cx1, cy1, g_ppTmp + nv, PP_OUT_CAP - nv);
            if (nv == 0) { g_ppStats.culled++; return 0; }
            res = g_ppTmp; resType = 4; resCount = (PP_U32)nv;
            g_ppStats.clipped++;
        }
        else g_ppStats.passed++;       // a type we cannot clip: scale only
    }
    else g_ppStats.passed++;

    // game coordinates -> real surface
    float ax, bx, ay, by;
    if (kind == PP_MAP_WIDE) { ax = g_scaleX;    bx = g_offX;  ay = g_scaleY;    by = g_offY;   }
    else                     { ax = g_scale43x;  bx = g_off43; ay = g_scale43y;  by = g_offY43; }
    for (i = 0; i < resCount; i++)
    {
        PP_SF(res[i], 0, bx + PP_F(res[i], 0) * ax);
        PP_SF(res[i], 1, by + PP_F(res[i], 1) * ay);
        memcpy(out + i * strideB, &res[i], strideB);
    }
    *outType  = resType;
    *outCount = resCount;
    return 1;
}
// ==== PP_PURE_END ============================================================

static int g_ppStatBudget = 40;

// Called once per frame (from the Flip hook).
static void PP_EndFrame()
{
    static int frames = 0;
    if (g_mode == 0 || !g_recon) return;
    if (++frames % 120 != 0 || g_ppStatBudget <= 0) return;
    g_ppStatBudget--;
    Log("[CANVAS %d] passed=%u clipped=%u culled=%u realsize=%u fullscreen_pictures=%u invalid=%u\n",
        frames, g_ppStats.passed, g_ppStats.clipped, g_ppStats.culled, g_ppStats.realsize,
        g_ppStats.backdrop, g_ppStats.invalid);
    memset(&g_ppStats, 0, sizeof(g_ppStats));
}

static HRESULT WINAPI hkDrawPrimitive(void* dev, DWORD type, DWORD fvf, void* verts, DWORD count, DWORD flags)
{
    if (g_recon) ReconDraw(type, fvf, verts, count);
    if (g_mode != 0)
    {
        static PP_U8 s_out[PP_OUT_CAP * PP_MAX_DW * 4];
        PP_U32 oType = 0, oCount = 0;
        int r = PP_Prepare((PP_U32)type, (PP_U32)fvf, verts, (PP_U32)count, s_out, &oType, &oCount);
        if (r > 0) return oDrawPrimitive(dev, (DWORD)oType, fvf, s_out, (DWORD)oCount, flags);
        if (r == 0) return 0;          // clipped away entirely: nothing to draw (S_OK)
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
        // The real viewport stays the whole surface: all clipping to the canvas is done
        // by the plugin (PP_Prepare), independent of how D3D treats XYZRHW vertices.
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

// IDirect3DDevice7::SetTextureStageState (vtable slot 37) - only used as an EARLY safe point:
// the exe calls it right after storing the real screen size in its context struct
// (exe VA 0x402cf1), before it builds any entity that would capture that size.
typedef HRESULT (WINAPI *D3DSetTSS_t)(void* dev, DWORD stage, DWORD type, DWORD value);
static D3DSetTSS_t oD3DSetTSS = nullptr;
static int  g_earlyCalls  = 0;

static HRESULT WINAPI hkD3DSetTextureStageState(void* dev, DWORD stage, DWORD type, DWORD value)
{
    // Only the first calls after SetDisplayMode matter (the exe stores the real size right
    // after it returns); the poke is idempotent, so doing it a few times is harmless.
    if (g_sdmReturned && g_earlyCalls < 64 && g_mode != 0)
    {
        g_earlyCalls++;
        ApplyVirtualScreen("SetTextureStageState hook (early)");
    }
    return oD3DSetTSS(dev, stage, type, value);
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

    void* fnDraw = nullptr; void* fnVp = nullptr; void* fnTr = nullptr; void* fnTss = nullptr;
    if (!ReadPtrSafe(&vtbl[25], &fnDraw) || !fnDraw) return;
    ReadPtrSafe(&vtbl[13], &fnVp);
    ReadPtrSafe(&vtbl[11], &fnTr);
    ReadPtrSafe(&vtbl[37], &fnTss);

    Log("[D3D] device found at %p (vtable %p): DrawPrimitive=%p SetViewport=%p SetTransform=%p SetTextureStageState=%p\n",
        dev, (void*)vtbl, fnDraw, fnVp, fnTr, fnTss);

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
    if (g_mode != 0 && fnTss && MH_CreateHook(fnTss, (LPVOID)&hkD3DSetTextureStageState, (LPVOID*)&oD3DSetTSS) == MH_OK)
    { MH_EnableHook(fnTss); Log("[D3D] hooked SetTextureStageState (early virtual screen size)\n"); }

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
// VIRTUAL CANVAS helpers
// ---------------------------------------------------------------------------
// exe global at VA 0x443d44 (RVA 0x43d44) points to a context struct whose +8/+0xC
// are the screen width/height used by all layout code (77 read sites). Tied to this
// exact exe build; we sanity-check the values before writing. First applied from
// the SetTextureStageState hook (the first D3D call the exe makes after storing the
// real size), then re-applied from the Agl7 hooks and every frame as a safety net.
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

// Returns true when the context struct holds the virtual size afterwards.
static bool ApplyVirtualScreen(const char* who)
{
    if (g_mode == 0) return false;
    BYTE* exeBase = (BYTE*)GetModuleHandleA(NULL);
    void* ctx = nullptr;
    if (!exeBase || !ReadPtrSafe(exeBase + 0x43d44, &ctx) || !ctx) return false;     // not created yet
    DWORD ow = 0, oh = 0;
    bool ok = PokeCtxDims(ctx, (DWORD)g_virtW, (DWORD)g_virtH, &ow, &oh);
    static int  changedLogs = 0;
    static bool heldLogged = false;
    static int  fails = 0;
    if (ok && (ow != (DWORD)g_virtW || oh != (DWORD)g_virtH))
    {
        if (changedLogs++ < 5)
            Log("[Virtual] screen context %p: %ux%u -> %dx%d (mode %d)  patched by: %s\n",
                ctx, ow, oh, g_virtW, g_virtH, g_mode, who ? who : "?");
    }
    else if (ok && !heldLogged)
    {
        heldLogged = true;
        Log("[Virtual] screen context %p already holds %dx%d  (checked by: %s)\n", ctx, ow, oh, who ? who : "?");
    }
    else if (!ok && !(ow == 0 && oh == 0) && fails++ < 3)      // 0x0 = exe has not stored the size yet
        Log("[Virtual] could not patch screen context %p (dims %ux%u) - unexpected values, skipped\n", ctx, ow, oh);
    return ok;
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
    PP_EndFrame();
    TryHookDevice(pThis);
    ApplyVirtualScreen();
    long r = oFlip(pThis, a, b);
    ClearBackBuffer(pThis);
    return r;
}

// ---------------------------------------------------------------------------
// PATCH: sky/background rectangle entity (exe-internal; only bytes to patch)
// ---------------------------------------------------------------------------
// At exe VA 0x420606/0x42060b the exe pushes hardcoded 480/640 literals as the size
// of a coloured background ("sky") rectangle entity (constructor VA 0x401e50).
// The immediates are replaced with the virtual canvas size. The original bytes are
// verified first, so a different exe build is left untouched.
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
    // RVA 0x20606 = "68 e0 01 00 00" (push 480); the immediate is at RVA+1
    PatchImmediateIfMatches(exeBase, 0x20607, (DWORD)ORIG_CLIENT_HEIGHT,
                             (DWORD)(g_mode != 0 ? g_virtH : g_targetHeight), "sky-rect height");
    // RVA 0x2060b = "68 80 02 00 00" (push 640)
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

    // --- user32.dll ---
    HMODULE hUser32 = GetModuleHandleA("user32.dll");
    if (hUser32)
    {
        void* target = GetProcAddress(hUser32, "AdjustWindowRectEx");
        if (target && MH_CreateHook(target, (LPVOID)&hkAdjustWindowRectEx, (void**)&oAdjustWindowRectEx) == MH_OK)
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
        if (pSetViewport && MH_CreateHook(pSetViewport, (LPVOID)&hkSetViewport, (void**)&oSetViewport) == MH_OK)
        {
            MH_EnableHook(pSetViewport);
            Log("Hooked AGLSCREEN::SetViewport\n");
        }
        else
        {
            Log("FAILED to hook AGLSCREEN::SetViewport (export not found?)\n");
        }

        void* pSetDisplayMode = GetProcAddress(hAgl, "?SetDisplayMode@AGLSCREEN@@QAEJKKEE@Z");
        if (pSetDisplayMode && MH_CreateHook(pSetDisplayMode, (LPVOID)&hkSetDisplayMode, (void**)&oSetDisplayMode) == MH_OK)
        {
            MH_EnableHook(pSetDisplayMode);
            Log("Hooked AGLSCREEN::SetDisplayMode\n");
        }
        else
        {
            Log("FAILED to hook AGLSCREEN::SetDisplayMode (export not found?)\n");
        }

        void* pCreateImage = GetProcAddress(hAgl, "?CreateImage@AGLSCREEN@@QAEPAVAGLIMAGE@@KKEG@Z");
        if (pCreateImage && MH_CreateHook(pCreateImage, (LPVOID)&hkCreateImage, (void**)&oCreateImage) == MH_OK)
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
            Log("Hooked AGLSCREEN::Flip (device discovery, per-frame work)\n");
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

    // --- exe-internal sky/background rectangle patch ---
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
        Log("PPWidescreenFix v1.5 loaded. Target resolution: %dx%d\n", g_targetWidth, g_targetHeight);
        Log("[Mode] %d  virtual canvas %dx%d  scale=%.4f,%.4f  offset=(%.1f,%.1f)  backdropFit=%d clip=%d clearBars=%d\n",
            g_mode, g_virtW, g_virtH, g_scaleX, g_scaleY, g_offX, g_offY, g_backdropFit, (int)g_clipToCanvas, (int)g_clearBars);
        InstallHooks();
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        MH_Uninitialize();
        if (g_log) fclose(g_log);
    }
    return TRUE;
}
