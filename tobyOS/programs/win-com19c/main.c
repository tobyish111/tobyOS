/* win-com19c/main.c -- Track C/C19c: a first-party proof that tobyOS implements
 * the genuine COM ABI (a TIGHTLY SCOPED slice of ole32 -- NOT a real COM
 * runtime). The kernel supports exactly one coclass (CLSID_TobyAdder) exposing
 * one interface (ITobyAdder : IUnknown) with one real method (Add). This program
 * imports the real ole32 entry points, drives the full lifecycle, and -- the
 * point of the milestone -- calls every interface method THROUGH THE OBJECT'S
 * VTABLE, which the kernel built out of marshalling gate-thunks. Exit 19 is
 * reachable only if CoTaskMem alloc/free, CoCreateInstance, vtable dispatch of
 * Add (6+7==42), AddRef/Release reference counting, and QueryInterface success
 * + E_NOINTERFACE all behaved correctly. */
#include <stdint.h>

typedef long          HRESULT;
typedef unsigned long ULONG;
typedef struct { uint8_t b[16]; } GUID;

/* GUIDs as raw 16-byte blobs the kernel matches byte-for-byte. */
static const GUID CLSID_TobyAdder = {{0x79,0x62,0x6F,0x74, 0x44,0x41, 0x52,0x44,
                                      'T','O','B','Y','C','L','S','D'}};
static const GUID IID_ITobyAdder  = {{0x79,0x62,0x6F,0x74, 0x49,0x44, 0x46,0x49,
                                      'T','O','B','Y','I','F','A','D'}};
static const GUID IID_IUnknown    = {{0,0,0,0, 0,0, 0,0,
                                      0xC0,0,0,0,0,0,0,0x46}};

typedef struct ITobyAdder ITobyAdder;
typedef struct ITobyAdderVtbl {
    HRESULT (*QueryInterface)(ITobyAdder *, const GUID *, void **);
    ULONG   (*AddRef)(ITobyAdder *);
    ULONG   (*Release)(ITobyAdder *);
    HRESULT (*Add)(ITobyAdder *, int, int, int *);
} ITobyAdderVtbl;
struct ITobyAdder { const ITobyAdderVtbl *lpVtbl; };

extern __declspec(dllimport) HRESULT __stdcall CoInitializeEx(void *, unsigned long);
extern __declspec(dllimport) void    __stdcall CoUninitialize(void);
extern __declspec(dllimport) HRESULT __stdcall CoCreateInstance(const GUID *, void *,
                                      unsigned long, const GUID *, void **);
extern __declspec(dllimport) void *  __stdcall CoTaskMemAlloc(uint64_t);
extern __declspec(dllimport) void    __stdcall CoTaskMemFree(void *);

int main(void) {
    if (CoInitializeEx(0, 0) != 0) return 1;             /* S_OK */

    /* CoTaskMemAlloc/Free round-trip onto the Win32 heap. */
    unsigned char *m = (unsigned char *)CoTaskMemAlloc(64);
    if (!m) return 2;
    int msum = 0;
    for (int i = 0; i < 64; i++) { m[i] = (unsigned char)i; }
    for (int i = 0; i < 64; i++) { msum += m[i]; }       /* 0+1+...+63 = 2016 */
    CoTaskMemFree(m);
    if (msum != 2016) return 3;

    /* Real coclass activation. */
    ITobyAdder *p = 0;
    HRESULT hr = CoCreateInstance(&CLSID_TobyAdder, 0, 1 /*CLSCTX_INPROC_SERVER*/,
                                  &IID_ITobyAdder, (void **)&p);
    if (hr != 0 || !p) return 4;

    /* The real interface method, dispatched through the vtable. */
    int sum = -1;
    hr = p->lpVtbl->Add(p, 20, 22, &sum);
    if (hr != 0 || sum != 42) return 5;

    /* IUnknown lifecycle through the vtable. */
    if (p->lpVtbl->AddRef(p) != 2) return 6;             /* ref 1 -> 2 */

    void *unk = 0;
    hr = p->lpVtbl->QueryInterface(p, &IID_IUnknown, &unk);  /* S_OK, ref -> 3 */
    if (hr != 0 || unk != (void *)p) return 7;

    void *bad = (void *)1;
    /* a GUID we don't expose as an interface -> E_NOINTERFACE, *ppv NULLed. */
    hr = p->lpVtbl->QueryInterface(p, &CLSID_TobyAdder, &bad);
    if (hr == 0 || bad != 0) return 8;

    p->lpVtbl->Release(p);                               /* 3 -> 2 */
    p->lpVtbl->Release(p);                               /* 2 -> 1 */
    if (p->lpVtbl->Release(p) != 0) return 9;            /* 1 -> 0 */

    CoUninitialize();
    return 19;                                           /* success */
}
