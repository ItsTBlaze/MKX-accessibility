/* Minimal dinput8.dll proxy + ASI loader.
   Forwards DirectInput to the real system dinput8.dll and loads every *.asi
   file that sits next to it (compatible with MKXHook's MKXHook.asi too). */
#include "win.h"

static HMODULE g_real;
typedef HRESULT (WINAPI *DI8Create_t)(void*, DWORD, const void*, void**, void*);
typedef HRESULT (WINAPI *Simple_t)(void);
typedef HRESULT (WINAPI *GetClassObj_t)(const void*, const void*, void**);

static void load_real(void)
{
    if (g_real) return;
    wchar path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (!n || n > MAX_PATH - 20) return;
    wcat(path, (const wchar*)L"\\dinput8.dll", MAX_PATH);
    g_real = LoadLibraryW(path);
}

__declspec(dllexport) HRESULT WINAPI DirectInput8Create(void* a, DWORD b, const void* c, void** d, void* e)
{
    load_real();
    DI8Create_t f = g_real ? (DI8Create_t)GetProcAddress(g_real, "DirectInput8Create") : 0;
    return f ? f(a, b, c, d, e) : (HRESULT)0x80004005;
}
__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void)
{
    load_real();
    Simple_t f = g_real ? (Simple_t)GetProcAddress(g_real, "DllCanUnloadNow") : 0;
    return f ? f() : 1;
}
__declspec(dllexport) HRESULT WINAPI DllGetClassObject(const void* a, const void* b, void** c)
{
    load_real();
    GetClassObj_t f = g_real ? (GetClassObj_t)GetProcAddress(g_real, "DllGetClassObject") : 0;
    return f ? f(a, b, c) : (HRESULT)0x80004005;
}
__declspec(dllexport) HRESULT WINAPI DllRegisterServer(void) { return (HRESULT)0x80004005; }
__declspec(dllexport) HRESULT WINAPI DllUnregisterServer(void) { return (HRESULT)0x80004005; }

static void load_asis(HMODULE self)
{
    wchar dir[MAX_PATH], pat[MAX_PATH], full[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    while (n && dir[n - 1] != '\\') n--;
    dir[n] = 0;
    wcpy(pat, dir, MAX_PATH); wcat(pat, (const wchar*)L"*.asi", MAX_PATH);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & 0x10) continue; /* directory */
        wcpy(full, dir, MAX_PATH); wcat(full, fd.cFileName, MAX_PATH);
        LoadLibraryW(full);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

BOOL WINAPI DllMain(HMODULE h, DWORD reason, void* r)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        load_asis(h);
    }
    return TRUE;
}
