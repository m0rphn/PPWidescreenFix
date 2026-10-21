// ============================================================================
// PPWidescreenFix v1.1 - Native resolution plugin for
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

static const int ORIG_CLIENT_WIDTH  = 640;
static const int ORIG_CLIENT_HEIGHT = 480;

static FILE* g_log = nullptr;

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
    g_forceViewport = GetPrivateProfileIntA("Settings", "ForceFullViewport", 1, INI_NAME) != 0;
    g_logResources  = GetPrivateProfileIntA("Settings", "LogResourceCalls", 0, INI_NAME) != 0;

    // If the ini doesn't exist yet, write out defaults so the user has
    // something to edit next time.
    if (GetFileAttributesA(INI_NAME) == INVALID_FILE_ATTRIBUTES)
    {
        WritePrivateProfileStringA("Settings", "Width", "1920", INI_NAME);
        WritePrivateProfileStringA("Settings", "Height", "1080", INI_NAME);
        WritePrivateProfileStringA("Settings", "ForceFullViewport", "1", INI_NAME);
        WritePrivateProfileStringA("Settings", "LogResourceCalls", "0", INI_NAME);
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

long __thiscall hkSetViewport(void* pThis, DWORD x, DWORD y, DWORD w, DWORD h)
{
    if (g_forceViewport)
    {
        Log("[SetViewport] game requested (%u,%u,%u,%u) -> forcing (0,0,%d,%d)\n",
            x, y, w, h, g_targetWidth, g_targetHeight);
        return oSetViewport(pThis, 0, 0, (DWORD)g_targetWidth, (DWORD)g_targetHeight);
    }
    return oSetViewport(pThis, x, y, w, h);
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

long __thiscall hkSetDisplayMode(void* pThis, DWORD width, DWORD height, BYTE bpp, BYTE flag)
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

void* __thiscall hkCreateImage(void* pThis, DWORD w, DWORD h, BYTE type, WORD flags)
{
    void* result = oCreateImage(pThis, w, h, type, flags);
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
                             (DWORD)g_targetHeight, "sky-rect height");

    // RVA 0x2060b = "68 80 02 00 00" (push 0x280 / 640), same pattern.
    PatchImmediateIfMatches(exeBase, 0x2060c, (DWORD)ORIG_CLIENT_WIDTH,
                             (DWORD)g_targetWidth, "sky-rect width");
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
        Log("PPWidescreenFix loaded. Target resolution: %dx%d\n", g_targetWidth, g_targetHeight);
        InstallHooks();
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        MH_Uninitialize();
        if (g_log) fclose(g_log);
    }
    return TRUE;
}
