#include <windows.h>
#include <stdio.h>

typedef int (__stdcall *D3DCreate9_t)(unsigned int);
typedef unsigned long (__stdcall *D3DCreate9Ex_t)(unsigned int, void **);
typedef int (__stdcall *Release_t)(void *);

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    HMODULE m = LoadLibraryA("d3d9.dll");
    if (!m) { printf("FAIL: LoadLibrary, err=%u\n", (unsigned)GetLastError()); return 1; }

    D3DCreate9_t c  = (D3DCreate9_t)GetProcAddress(m, "Direct3DCreate9");
    D3DCreate9Ex_t x = (D3DCreate9Ex_t)GetProcAddress(m, "Direct3DCreate9Ex");
    if (!c) { printf("FAIL: Direct3DCreate9 missing\n"); return 1; }
    printf("proxy loaded, Direct3DCreate9@%p, Ex=%s\n",
           (void *)c, x ? "present" : "absent");

    void *dev = (void *)c(32 /*D3D_SDK_VERSION*/);
    if (!dev) { printf("NOTE: Create9 returned NULL (GPU-less env), no crash = OK\n"); }
    else {
        printf("IDirect3D9 ok @%p\n", dev);
        /* vtable sanity: Release() on wrapper object */
        void **vt = *(void ***)dev;
        Release_t rel = (Release_t)vt[2];      /* IUnknown::Release */
        int refs = rel(dev);
        printf("Release() -> %d\n", refs);
    }
    return 0;
}