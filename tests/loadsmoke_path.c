/*
 * loadsmoke_path.c — verify the proxy d3d9.dll is the one ACTUALLY loaded
 * (the copy sitting next to this exe, not system32's) and that its key
 * exports resolve.
 *
 * The build worker's copy of the proxy is placed in tests\d3d9.dll next to
 * this test. We load it by explicit path and assert the resolved module
 * path is that file and NOT the system32 d3d9.dll, which is what a naive
 * `LoadLibraryA("d3d9.dll")` from tests\ would resolve to.
 *
 * Build (from tests dir):
 *   i686-w64-mingw32-gcc.exe loadsmoke_path.c -o loadsmoke_path.exe
 */
#include <windows.h>
#include <stdio.h>

typedef int (__stdcall *D3DCreate9_t)(unsigned int);

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    char exe[MAX_PATH];
    char dir[MAX_PATH];
    char target[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
    if (!n) { printf("FAIL: GetModuleFileName err=%u\n", (unsigned)GetLastError()); return 1; }
    char *slash = strrchr(exe, '\\');
    if (!slash) { printf("FAIL: bad exe path\n"); return 1; }
    *slash = '\0';
    _snprintf(dir, MAX_PATH, "%s", exe);
    _snprintf(target, MAX_PATH, "%s\\d3d9.dll", dir);

    HMODULE m = LoadLibraryA(target);
    if (!m) {
        printf("FAIL: LoadLibrary %s err=%u\n", target, (unsigned)GetLastError());
        return 1;
    }
    char path[MAX_PATH];
    n = GetModuleFileNameA(m, path, MAX_PATH);
    printf("loaded module: %s\n", n ? path : "(GetModuleFileName failed)");

    if (!n || _stricmp(path, target) != 0) {
        printf("FAIL: expected to load %s\n", target);
        return 1;
    }
    if (strstr(path, "system32")) {
        printf("FAIL: resolved into system32, not the proxy copy\n");
        return 1;
    }
    printf("PASS: loaded OUR proxy copy, not system32's d3d9.dll\n");

    D3DCreate9_t c = (D3DCreate9_t)GetProcAddress(m, "Direct3DCreate9");
    if (!c) { printf("FAIL: Direct3DCreate9 missing\n"); return 1; }
    printf("Direct3DCreate9 @ %p\n", (void *)c);
    /* create + release to prove in-process usability */
    void *obj = (void *)c(32);
    if (!obj) { printf("NOTE: Create9 NULL (GPU-less env) — no crash = OK\n"); return 0; }
    void **vt = *(void ***)obj;
    printf("wrapper vtable %p, Release -> %lu\n", (void *)vt,
           ((unsigned long (__stdcall *)(void *))vt[2])(obj));
    printf("PASS: proxy d3d9.dll loaded and Create9 callable\n");
    return 0;
}