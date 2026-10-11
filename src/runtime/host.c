/*
 * Monster Truck Madness 2 - static recompilation host.
 *
 * A 32-bit host on pcrecomp's runtime/native32 (the native bridge, callbacks
 * and the machine lock; see its header). What is here is only what is
 * specific to this game: the guest image, where it finds its data, the
 * command line, scripted input and the fault report. The display is
 * ddraw.c. docs/host.md has the reasoning.
 *
 * Linked at /BASE:0x60000000 (CMakeLists.txt) so 0x00400000 (Monster.exe) is
 * free when main() maps the image.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "native32.h"
#include "recomp_trace.h"
#include "ddraw_host.h"
#include "present.h"

extern const uint32_t mtm_entry_va;       /* recomp_dispatch.c */

#define MTM_BASE 0x00400000u

static DWORD g_watchdog_s;
static int   g_headless;
static char  g_game[MAX_PATH];           /* the game folder, with a trailing '\' */
static char  g_guest_exe[MAX_PATH], g_guest_cmdline[MAX_PATH + 3];

#define ARG(n) MEM32(g_esp + 4 + 4 * (n))
#define RET(v, nargs) do { g_eax = (uint32_t)(v); g_esp += 4 + 4 * (nargs); } while (0)
static const char* gstr(uint32_t va) { return va ? (const char*)(uintptr_t)va : "(null)"; }

/* ------------------------------------------------------------- the guest */

/* The guest is Monster.exe in the game folder, not this host: its hInstance
 * (icon, strings) is GetModuleHandleA(NULL). */
static void shim_GetModuleHandleA(void) {
    uint32_t name = ARG(0);
    RET(name ? (uint32_t)(uintptr_t)GetModuleHandleA(gstr(name)) : MTM_BASE, 1);
}

static void shim_GetModuleFileNameA(void) {
    uint32_t h = ARG(0), size = ARG(2);
    char* out = (char*)(uintptr_t)ARG(1);
    if (h == 0 || h == MTM_BASE) {
        uint32_t n = (uint32_t)strlen(g_guest_exe);
        if (size) {
            if (n >= size) n = size - 1;
            memcpy(out, g_guest_exe, n);
            out[n] = 0;
        }
        RET(n, 3);
    } else {
        RET(GetModuleFileNameA((HMODULE)(uintptr_t)h, out, size), 3);
    }
}

static void shim_GetCommandLineA(void) { RET((uintptr_t)g_guest_cmdline, 0); }

/* --headless: nothing reaches the screen (REPO_RULES section 13). The game's
 * own message boxes go to stderr. */
static void shim_MessageBoxA(void) {
    fprintf(stderr, "[messagebox] %s: %s\n", gstr(ARG(2)), gstr(ARG(1)));
    RET(IDOK, 4);
}

/* ------------------------------------------------------------ input */

/* --key NAME@MS[+HOLD] and --click X,Y@MS: scripted input, in milliseconds
 * from entry. The game reads keys through window messages and GetKeyState
 * (DirectInput is created but no device on it), so a scripted key is held
 * for GetKeyState and posted to the focus window; a click is posted as a
 * button down and up at a 640x480 position. Headless, the real keyboard is not read at all. */
#define MAX_INPUT 64
static struct { int vk, x, y; DWORD at, hold; } g_in[MAX_INPUT];
static int g_nin;
static volatile LONG g_held[256];
static int vk_of(const char* s) {
    static const struct { const char* n; int vk; } names[] = {
        {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT},
        {"SPACE", VK_SPACE}, {"ENTER", VK_RETURN}, {"ESC", VK_ESCAPE}, {"SHIFT", VK_SHIFT},
        {"CTRL", VK_CONTROL}, {"TAB", VK_TAB}, {"LBUTTON", VK_LBUTTON}};
    for (int i = 0; i < (int)(sizeof names / sizeof names[0]); i++)
        if (!_stricmp(s, names[i].n)) return names[i].vk;
    if ((s[0] == 'F' || s[0] == 'f') && atoi(s + 1) >= 1 && atoi(s + 1) <= 12) return VK_F1 + atoi(s + 1) - 1;
    if (s[0] && !s[1]) return toupper((unsigned char)s[0]);
    return (int)strtol(s, NULL, 0);
}
static int parse_key(const char* arg) {
    char name[16];
    unsigned at = 0, hold = 100;
    if (g_nin >= MAX_INPUT || sscanf(arg, "%15[^@]@%u+%u", name, &at, &hold) < 2) return 0;
    g_in[g_nin].vk = vk_of(name) & 0xFF;
    g_in[g_nin].x = -1;
    g_in[g_nin].at = at;
    g_in[g_nin++].hold = hold;
    return 1;
}
static int parse_click(const char* arg) {
    int x, y;
    unsigned at = 0;
    if (g_nin >= MAX_INPUT || sscanf(arg, "%d,%d@%u", &x, &y, &at) != 3) return 0;
    g_in[g_nin].vk = VK_LBUTTON;
    g_in[g_nin].x = x;
    g_in[g_nin].y = y;
    g_in[g_nin].at = at;
    g_in[g_nin++].hold = 80;
    return 1;
}
static DWORD WINAPI input_thread(LPVOID p) {
    int i = (int)(intptr_t)p;
    DWORD t0 = GetTickCount();
    while (!ddh_window() || GetTickCount() - t0 < g_in[i].at) Sleep(5);
    /* A modal dialog up (GO's "Before You Begin") takes the input: clicks
     * stay in main-window coordinates and are mapped onto it. */
    HWND w = ddh_window(), pop = GetLastActivePopup(w);
    int vk = g_in[i].vk;
    if (g_in[i].x >= 0) {
        int cx = g_in[i].x, cy = g_in[i].y;
        present_unmap(&cx, &cy);            /* the presenter maps it back to X,Y */
        /* The front end's buttons are MFC child windows: the click goes to
         * the deepest child under the point, in its own coordinates. */
        POINT pt = { cx, cy };
        if (pop != w) { MapWindowPoints(w, pop, &pt, 1); w = pop; }
        for (HWND c; (c = RealChildWindowFromPoint(w, pt)) && c != w; w = c)
            MapWindowPoints(w, c, &pt, 1);
        LPARAM xy = MAKELPARAM(pt.x, pt.y);
        fprintf(stderr, "[input] click %d,%d at %lu ms -> hwnd %p at %ld,%ld\n", g_in[i].x, g_in[i].y,
                GetTickCount() - t0, (void*)w, pt.x, pt.y);
        PostMessageA(w, WM_MOUSEMOVE, 0, xy);
        Sleep(30);
        InterlockedIncrement(&g_held[VK_LBUTTON]);
        PostMessageA(w, WM_LBUTTONDOWN, MK_LBUTTON, xy);
        Sleep(g_in[i].hold);
        InterlockedDecrement(&g_held[VK_LBUTTON]);
        PostMessageA(w, WM_LBUTTONUP, 0, xy);
        return 0;
    }
    /* Keys go where real ones would: the game thread's focus window (the
     * race's view, not the MDI frame), else the popup. In a dialog,
     * IsDialogMessage turns ENTER into the default button. */
    GUITHREADINFO gi = { sizeof gi };
    w = GetGUIThreadInfo(GetWindowThreadProcessId(pop, NULL), &gi) && gi.hwndFocus ? gi.hwndFocus : pop;
    fprintf(stderr, "[input] key 0x%02X down at %lu ms -> hwnd %p\n", vk, GetTickCount() - t0, (void*)w);
    InterlockedIncrement(&g_held[vk]);
    PostMessageA(w, WM_KEYDOWN, vk, 1);
    Sleep(g_in[i].hold);
    InterlockedDecrement(&g_held[vk]);
    PostMessageA(w, WM_KEYUP, vk, 0xC0000001u);
    return 0;
}
static void shim_GetKeyState(void) {
    int vk = ARG(0) & 0xFF;
    SHORT s = g_held[vk] ? (SHORT)0x8000 : g_headless ? 0 : GetKeyState(vk);
    RET((uint16_t)s, 1);
}

/* ------------------------------------------------------------ exit */

static void shim_ExitProcess(void) {
    fprintf(stderr, "[exit] ExitProcess(%u) after %ld frames\n", ARG(0), ddh_frames());
    ddh_finish();
    fflush(stderr);
    ExitProcess(ARG(0));
}

/* The front end fetches ChangeDisplaySettingsA with GetProcAddress (sub_0058B310),
 * around the import table, and switches the real desktop to 640x480 and back
 * as its window gains and loses focus: on a cloaked headless window, over and
 * over. The display is the host's (ddraw.c), so the game's mode change is only
 * logged. */
static LONG WINAPI fake_ChangeDisplaySettingsA(DEVMODEA* dm, DWORD flags) {
    if (dm) fprintf(stderr, "[display] ChangeDisplaySettingsA(%lux%lux%lu, %lx): not applied\n",
                    dm->dmPelsWidth, dm->dmPelsHeight, dm->dmBitsPerPel, flags);
    else fprintf(stderr, "[display] ChangeDisplaySettingsA(restore): not applied\n");
    return DISP_CHANGE_SUCCESSFUL;
}
static void shim_GetProcAddress(void) {
    uint32_t name = ARG(1);
    if (name >= 0x10000 && !strcmp((const char*)(uintptr_t)name, "ChangeDisplaySettingsA")) {
        RET((uintptr_t)fake_ChangeDisplaySettingsA, 2);
        return;
    }
    RET((uintptr_t)GetProcAddress((HMODULE)(uintptr_t)ARG(0), (LPCSTR)(uintptr_t)name), 2);
}

/* The front end idles in GetMessageA, and loading and racing poll with
 * PeekMessageA. Either with nothing in the queue means every control has
 * repainted, so --record may read the window back then (ddraw.c). These are
 * native functions in the guest's IAT, not shims: the bridge calls them like
 * any import and releases the machine lock while they block, so the mixer
 * thread keeps running. */
static BOOL WINAPI host_GetMessageA(MSG* m, HWND h, UINT lo, UINT hi) {
    ddh_ui_idle(!HIWORD(GetQueueStatus(QS_ALLINPUT)));
    BOOL got = GetMessageA(m, h, lo, hi);
    ddh_ui_idle(0);
    return got;
}
static BOOL WINAPI host_PeekMessageA(MSG* m, HWND h, UINT lo, UINT hi, UINT rm) {
    BOOL got = PeekMessageA(m, h, lo, hi, rm);
    ddh_ui_idle(!got);
    return got;
}

/* Point every IAT slot of the image at base that holds `real` at `repl`. */
static int patch_iat(uint32_t base, void* real, void* repl) {
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(uintptr_t)(base + ((IMAGE_DOS_HEADER*)(uintptr_t)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT];
    uint32_t* slot = (uint32_t*)(uintptr_t)(base + d.VirtualAddress);
    int n = 0;
    for (DWORD i = 0; i < d.Size / 4; i++)
        if (slot[i] == (uint32_t)(uintptr_t)real) { slot[i] = (uint32_t)(uintptr_t)repl; n++; }
    return n;
}

static native32_shim_t g_shims[] = {
    { "GetProcAddress", shim_GetProcAddress },
    { "GetModuleHandleA", shim_GetModuleHandleA },
    { "GetModuleFileNameA", shim_GetModuleFileNameA },
    { "GetCommandLineA", shim_GetCommandLineA },
    { "GetKeyState", shim_GetKeyState },
    { "ExitProcess", shim_ExitProcess },
    DDH_SHIMS,
    { "MessageBoxA", shim_MessageBoxA },          /* headless only: the last entry */
};

/* ------------------------------------------------------------ reports */

recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }

/* Added after native32's own handler, so callbacks are resolved first and
 * only real faults get here. */
static LONG CALLBACK crash(EXCEPTION_POINTERS* ep) {
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if ((r->ExceptionCode & 0xF0000000u) != 0xC0000000u) return EXCEPTION_CONTINUE_SEARCH;
    fprintf(stderr, "\n=== fault 0x%08lX at 0x%p ===\n", r->ExceptionCode, r->ExceptionAddress);
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        ULONG_PTR op = r->ExceptionInformation[0];
        uint32_t at = (uint32_t)r->ExceptionInformation[1];
        fprintf(stderr, "  %s of 0x%08X%s\n", op == 0 ? "read" : op == 1 ? "write" : "execute", at,
                native32_in_guest(at) ? " (inside the guest image)" : at < 0x10000 ? " (null/low)" : "");
    }
    recomp_trace_flush();
    fprintf(stderr, "  in lifted sub_%08X, last native call %s\n", g_cur_func, g_cur_import);
    fprintf(stderr, "  eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi);
    native32_dump_icalls(12);
    recomp_dump_trace("fault");
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Where every other thread is: its eip and the host return addresses on its
 * stack, newest first. `py -3 tools/addr2line.py` names them. */
static void dump_threads(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = { sizeof te };
    uintptr_t lo = (uintptr_t)GetModuleHandleA(NULL), hi = lo + 0x4000000;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
        CONTEXT c = { 0 };
        c.ContextFlags = CONTEXT_CONTROL;
        if (t && SuspendThread(t) != (DWORD)-1) {
            if (GetThreadContext(t, &c)) {
                fprintf(stderr, "  thread %lu: eip=%08lX esp=%08lX host frames:", te.th32ThreadID, c.Eip, c.Esp);
                MEMORY_BASIC_INFORMATION mbi;
                int n = 0;
                if (VirtualQuery((void*)(uintptr_t)c.Esp, &mbi, sizeof mbi))
                    for (uint32_t* p = (uint32_t*)(uintptr_t)c.Esp;
                         (uint8_t*)p + 4 <= (uint8_t*)mbi.BaseAddress + mbi.RegionSize && n < 16; p++)
                        if (*p >= lo && *p < hi) { fprintf(stderr, " %08X", *p); n++; }
                fprintf(stderr, "\n");
            }
            ResumeThread(t);
        }
        if (t) CloseHandle(t);
    }
    CloseHandle(snap);
}

static DWORD WINAPI watchdog(LPVOID unused) {
    (void)unused;
    Sleep(g_watchdog_s * 1000);
    dump_threads();
    fprintf(stderr, "\n[watchdog] %lu s: in sub_%08X, last native call %s, %u indirect calls, %ld frames\n",
            g_watchdog_s, g_cur_func, g_cur_import, g_icall_count, ddh_frames());
    native32_dump_icalls(8);
    recomp_dump_trace("watchdog");          /* with -DMTM_TRACE=ON: the last function entries */
    ddh_finish();
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 4);
    return 0;
}

int main(int argc, char** argv) {
    const char* game = "game";
    int run = 0, scale = 0;
    ddh_opts_t o = { 0 };
    for (int i = 1; i < argc; i++) {
        int n = recomp_trace_arg(argc, argv, i);
        if (n) { i += n - 1; continue; }
        if (!strcmp(argv[i], "--run")) run = 1;
        else if (!strcmp(argv[i], "--headless")) g_headless = 1;
        else if (!strcmp(argv[i], "--key") && i + 1 < argc) {
            if (!parse_key(argv[++i])) { fprintf(stderr, "bad --key %s\n", argv[i]); return 1; }
        }
        else if (!strcmp(argv[i], "--click") && i + 1 < argc) {
            if (!parse_click(argv[++i])) { fprintf(stderr, "bad --click %s (want X,Y@MS)\n", argv[i]); return 1; }
        }
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) {
            /* Absolute now: --run moves into the game folder before ffmpeg starts. */
            static char rec[MAX_PATH];
            GetFullPathNameA(argv[++i], MAX_PATH, rec, NULL);
            o.record = rec;
        }
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) o.stop_after = atol(argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) {
            static char shot[MAX_PATH];
            char* colon = strchr(argv[++i], ':');
            o.shot_frame = atol(argv[i]);
            if (!colon || o.shot_frame < 1) { fprintf(stderr, "bad --shot %s (want N:file.bmp)\n", argv[i]); return 1; }
            GetFullPathNameA(colon + 1, MAX_PATH, shot, NULL);
            o.shot_path = shot;
        }
        else if (!strcmp(argv[i], "--diff") && i + 1 < argc) {
            if (sscanf(argv[++i], "%ld,%ld", &o.diff_a, &o.diff_b) != 2 || o.diff_b <= o.diff_a) {
                fprintf(stderr, "bad --diff %s (want A,B in ms, A < B)\n", argv[i]);
                return 1;
            }
        }
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc) g_watchdog_s = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--native-trace")) native32_trace_native = 1;
        else if (!strcmp(argv[i], "--callbacks")) native32_trace_callbacks = 1;
        else if (!strcmp(argv[i], "--ddraw-trace")) o.trace = 1;
        else {
            printf("usage: mtm [--run] [--headless] [--record out.mp4] [--frames N]\n"
                   "             [--shot N:file.bmp] [--diff A,B] [--key NAME@MS[+HOLD]] [--click X,Y@MS]\n"
                   "             [--scale N] [--game game] [--watchdog S] [--native-trace]\n"
                   "             [--callbacks] [--ddraw-trace]\n");
            recomp_trace_help();
            return argv[i][1] == 'h' || argv[i][2] == 'h' ? 0 : 1;
        }
    }
    GetFullPathNameA(game, MAX_PATH - 1, g_game, NULL);
    if (g_game[strlen(g_game) - 1] != '\\') strcat(g_game, "\\");
    _snprintf(g_guest_exe, sizeof g_guest_exe - 1, "%sMonster.exe", g_game);
    _snprintf(g_guest_cmdline, sizeof g_guest_cmdline - 1, "\"%s\"", g_guest_exe);

    o.headless = g_headless;
    o.scale = scale;
    ddh_init(&o);
    native32_init();
    SetDllDirectoryA(g_game);            /* smackw32.dll and the renderer DLLs sit in the game folder */
    AddVectoredExceptionHandler(0, crash);
    printf("Monster Truck Madness 2 recomp host\n  lifted functions in dispatch: %u\n", recomp_dispatch_count);
    uint32_t span = native32_map(g_guest_exe, MTM_BASE);
    if (!span) { fprintf(stderr, "cannot map %s at 0x%08X\n", g_guest_exe, MTM_BASE); return 1; }
    printf("  mapped Monster.exe: 0x%08X-0x%08X\n", MTM_BASE, MTM_BASE + span);
    int nshims = (int)(sizeof g_shims / sizeof g_shims[0]) - !g_headless;
    if (native32_bind(MTM_BASE, g_shims, nshims) != 0) return 1;
    if (!patch_iat(MTM_BASE, (void*)GetProcAddress(GetModuleHandleA("user32.dll"), "GetMessageA"), host_GetMessageA))
        fprintf(stderr, "[bind] GetMessageA not in the IAT: the menu will not reach --record\n");
    patch_iat(MTM_BASE, (void*)GetProcAddress(GetModuleHandleA("user32.dll"), "PeekMessageA"), host_PeekMessageA);
    if (!run) {
        printf("\n(dry run: image mapped and bound; --run enters 0x%08X)\n", mtm_entry_va);
        return 0;
    }
    /* The game opens its PODs and INI by bare name, from the current folder. */
    if (!SetCurrentDirectoryA(g_game)) { fprintf(stderr, "cannot enter %s\n", g_game); return 1; }
    if (g_watchdog_s) CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
    for (int i = 0; i < g_nin; i++)
        CloseHandle(CreateThread(NULL, 0, input_thread, (LPVOID)(intptr_t)i, 0, NULL));
    printf("  entering 0x%08X\n\n", mtm_entry_va);
    fflush(stdout);
    native32_call_guest(mtm_entry_va, 0, NULL);
    printf("\nentry returned eax=%08X\n", g_eax);
    ddh_finish();
    return (int)g_eax;
}
