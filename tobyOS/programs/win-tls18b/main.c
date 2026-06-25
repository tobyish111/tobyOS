/* C18b first-party test: compiler thread-local storage (__declspec(thread) /
 * _Thread_local) via the PE .tls directory + per-thread blocks.
 *
 * Built with -fno-emulated-tls so clang emits NATIVE Windows TLS:
 *   mov eax,[_tls_index]; mov rcx,gs:[0x58]; mov rsi,[rcx+rax*8]; ...[rsi+off]
 * which only works if the loader laid out a TLS template block, wrote
 * _tls_index, and pointed each thread's TEB+0x58 at a private pointer array.
 *
 * Proof (exit code 42 reachable ONLY with genuine per-thread TLS):
 *   - main mutates its own copy to 1005;
 *   - each of 4 worker threads must observe the FRESH template (g_n==1000,
 *     g_zero==0, g_tag=="tls") at entry -- proving its block was template-
 *     initialised and is NOT shared with main or its siblings -- then mutates
 *     privately;
 *   - main re-checks its copy is still 1005 / "tls" (workers did not stomp it)
 *     and that every worker produced 1000+(id+1)*7.
 * Shared TLS (one block) or an unset gs:[0x58] would corrupt these and fail.
 *
 * Deliberately uses only printf (the shimmed __stdio_common_vfprintf) and
 * direct byte ops -- no snprintf/strcmp -- to keep the test about TLS, not the
 * ucrt string backend. */
#include <windows.h>
#include <stdio.h>

_Thread_local int  g_n   = 1000;          /* template initial value      */
_Thread_local char g_tag[16] = "tls";     /* template string (raw bytes) */
_Thread_local long g_zero;                /* zero-fill region            */

#define NTHREADS 4
static volatile int g_result[NTHREADS];   /* plain shared global (not TLS) */

static int tag_is_tls(void) {
    return g_tag[0] == 't' && g_tag[1] == 'l' && g_tag[2] == 's' && g_tag[3] == 0;
}

static DWORD WINAPI worker(LPVOID param) {
    int id = (int)(INT_PTR)param;
    /* At entry each thread must see the pristine template, not another
     * thread's mutations -- this is the per-thread isolation proof. */
    int fresh = (g_n == 1000) && (g_zero == 0) && tag_is_tls();
    g_n += (id + 1) * 7;                   /* mutate this thread's private copy */
    g_zero = id + 1;
    g_tag[0] = 'W';                        /* private mutation (no CRT needed)  */
    for (volatile int i = 0; i < 300000; i++) { }   /* overlap the threads */
    g_result[id] = (fresh && g_zero == (long)(id + 1) && g_tag[0] == 'W')
                       ? g_n : -1;
    return 0;
}

int main(void) {
    g_n += 5;                              /* main's private copy -> 1005 */

    HANDLE h[NTHREADS];
    for (int i = 0; i < NTHREADS; i++)
        h[i] = CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)i, 0, NULL);
    for (int i = 0; i < NTHREADS; i++)
        if (h[i]) WaitForSingleObject(h[i], INFINITE);

    int ok = (g_n == 1005) && (g_zero == 0) && tag_is_tls();  /* main untouched */
    for (int i = 0; i < NTHREADS; i++)
        ok = ok && (g_result[i] == 1000 + (i + 1) * 7);

    printf("[tls18b] main g_n=%d tag0=%c zero=%ld results=%d,%d,%d,%d ok=%d\n",
           g_n, g_tag[0], g_zero,
           g_result[0], g_result[1], g_result[2], g_result[3], ok);
    fflush(stdout);
    return ok ? 42 : 1;
}
