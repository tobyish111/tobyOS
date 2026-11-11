/* life30.h -- Track C, W5: what win-life30.exe and its DLLs share. life30a.dll
 * keeps one log of every DllMain and TLS-callback call the others make, in
 * the order they happen, with the calling thread and the caller's own
 * thread-local value at that moment. */
#ifndef LIFE30_H
#define LIFE30_H
#include <windows.h>

#ifdef LIFE30A_BUILD
#define L30API __declspec(dllexport)
#else
#define L30API __declspec(dllimport)
#endif

struct l30_ev {
    char          who;      /* 'A' 'C' 'D' 'S' a DllMain, 'a' 'd' 'X' a TLS callback */
    unsigned char reason;   /* DLL_PROCESS_DETACH 0, ATTACH 1, THREAD_ATTACH 2, DETACH 3 */
    unsigned char res;      /* lpReserved != NULL */
    unsigned char pad;
    DWORD         tid;      /* GetCurrentThreadId() of the call */
    int           tls;      /* the caller's thread-local variable, read in the call */
};

/* The checks of the process's exit (armed by the .exe just before
 * ExitProcess; a_exit_check makes them): the events since a_arm_exit, in
 * order. */
struct l30_exit {
    const char   *seq;      /* e.g. "d0 D0r C0r X0": who + reason (+ r: lpReserved
                             * non-NULL, n: NULL), in order */
    DWORD         tid;      /* the thread that ends the process */
    volatile LONG *spin;    /* a counter another thread keeps incrementing */
    DWORD         spin_tid; /* that thread: it must get no DLL_THREAD_DETACH */
};

L30API void  a_log(char who, DWORD reason, void *res, int tls);
L30API int   a_log_n(void);
L30API int   a_log_get(int i, struct l30_ev *out);
L30API void  a_log_reset(void);
L30API void  a_logging(int on);
L30API LONG  a_count(char who, DWORD reason);
L30API int   a_get_tls(void);
L30API void  a_set_tls(int v);
L30API DWORD a_tls_index(void);
L30API void  a_arm_exit(const struct l30_exit *e);
L30API int   a_exit_check(void);
/* How long life30s's DllMain sleeps at DLL_PROCESS_ATTACH, in ms. */
L30API void  a_set_param(int ms);
L30API int   a_get_param(void);

#endif
