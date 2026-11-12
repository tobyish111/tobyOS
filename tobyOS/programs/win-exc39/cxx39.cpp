// win-cxx39 -- Track C, W7h: C++ built by Microsoft's compiler (cl /EHs /MD:
// /EHs, not /EHsc -- a throw that passes a C function, DispatchMessage in
// check 9, needs it; under /EHsc the compiler assumes extern "C" never
// throws, and on Windows the throw lands in the wrong catch)
// on Microsoft's redistributable C++ runtime -- vcruntime140.dll (throw:
// _CxxThrowException) and vcruntime140_1.dll (__CxxFrameHandler4, which
// unwinds to a catch block with STATUS_UNWIND_CONSOLIDATE) -- as Windows 11
// runs it (measured first: it exits 41 there) -- a throw out of a window
// procedure through DispatchMessage among the checks (W7g's stubs). Exit 41
// iff all pass, else 100 + the first that failed. Kept to vcruntime and the
// UCRT (no msvcp140): its own exception classes, printf, a Win32 thread.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <exception>

static char g_log[2048];
static void lg(const char *s) { strcat_s(g_log, sizeof g_log, s); }

static int first_fail;
static void check(int n, bool ok, const char *what) {
    printf("%s %2d %s\n", ok ? "ok  " : "FAIL", n, what);
    if (!ok && !first_fail) first_fail = n;
    if (!ok) printf("        log: %s\n", g_log);
}

struct Err : std::exception {
    int code;
    explicit Err(int c) : code(c) {}
    const char *what() const noexcept override { return "Err"; }
};
struct Sub : Err {
    explicit Sub(int c) : Err(c) {}
    const char *what() const noexcept override { return "Sub"; }
};

static int g_live;
struct Guard {
    const char *name;
    explicit Guard(const char *n) : name(n) { g_live++; }
    ~Guard() { g_live--; lg("~"); lg(name); lg(" "); }
};

__declspec(noinline) static void thrower(int kind) {
    Guard g("t");
    if (kind == 0) throw 7;
    if (kind == 1) throw Err(11);
    if (kind == 2) throw Sub(22);
    if (kind == 3) throw "text";
}
__declspec(noinline) static void middle(int kind) {
    Guard g("m");
    thrower(kind);
    lg("not ");
}

// 1: the kinds, each caught where it should be
static int catch_kind(int kind) {
    try {
        middle(kind);
    } catch (int v) {
        return 100 + v;
    } catch (Sub &s) {
        return 300 + s.code;
    } catch (const Err &e) {
        return 200 + e.code + (strcmp(e.what(), "Err") == 0 ? 0 : 1000);
    } catch (const char *s) {
        return strcmp(s, "text") == 0 ? 400 : 401;
    }
    return 0;
}

// 4: deep, every frame with a destructor
__declspec(noinline) static int deep(int n) {
    Guard g("d");
    if (n == 0) throw Err(n + 99);
    return deep(n - 1) + 1;
}

// 5: a destructor run by the unwind, with a try/catch of its own inside it
struct Inner {
    ~Inner() {
        lg(__uncaught_exceptions() == 1 ? "unwinding " : "not-unwinding ");
        try {
            throw 5;
        } catch (int v) {
            lg(v == 5 ? "inner-caught " : "inner-wrong ");
        }
    }
};
__declspec(noinline) static void with_inner() {
    Inner i;
    throw Err(3);
}

// 7: on another thread
static DWORD WINAPI thread_main(void *) {
    try {
        middle(1);
    } catch (const Err &e) {
        return (DWORD)e.code;
    }
    return 0;
}

// 9: a window procedure that throws
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_USER) {
        Guard g("w");
        throw Err(77);
    }
    return DefWindowProcA(h, m, w, l);
}

// 8: the children (they write with WriteFile: the bytes, no text-mode newlines)
static void say(const char *s) {
    DWORD n;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, (DWORD)strlen(s), &n, 0);
}
static int child(const char *m) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    say("child\n");
    if (!strcmp(m, "uncaught")) middle(1);
    say("child: went on\n");
    return 4;
}
static int run_child(const char *self, const char *args, char *out, int cap, DWORD *code) {
    SECURITY_ATTRIBUTES sa = { sizeof sa, 0, TRUE };
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    char cmd[400];
    strcpy_s(cmd, sizeof cmd, "\"");
    strcat_s(cmd, sizeof cmd, self);
    strcat_s(cmd, sizeof cmd, "\" ");
    strcat_s(cmd, sizeof cmd, args);
    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = wr;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(self, cmd, 0, 0, TRUE, 0, 0, 0, &si, &pi)) { CloseHandle(rd); CloseHandle(wr); return -1; }
    CloseHandle(wr);
    int n = 0;
    for (;;) {
        DWORD got = 0;
        if (n >= cap - 1 || !ReadFile(rd, out + n, (DWORD)(cap - 1 - n), &got, 0) || !got) break;
        n += (int)got;
    }
    out[n] = 0;
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 30000);
    GetExitCodeProcess(pi.hProcess, code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return n;
}

int main(int argc, char **argv) {
    if (argc > 1) return child(argv[1]);
    printf("win-cxx39: C++ by cl.exe on vcruntime140\n");

    g_log[0] = 0;
    int r0 = catch_kind(0), r1 = catch_kind(1), r2 = catch_kind(2), r3 = catch_kind(3);
    check(1, r0 == 107 && r1 == 211 && r2 == 322 && r3 == 400 && g_live == 0 &&
             !strcmp(g_log, "~t ~m ~t ~m ~t ~m ~t ~m "),
          "throw int, a class, a subclass, a string: each caught by the right catch; destructors run in order");

    g_log[0] = 0;
    int r = 0;
    try {
        try {
            middle(1);
        } catch (const Err &e) {
            lg("first ");
            r = e.code;
            throw;
        }
    } catch (std::exception &e) {
        r += strcmp(e.what(), "Err") == 0 ? 1000 : 0;
    }
    check(2, r == 1011 && !strcmp(g_log, "~t ~m first "), "throw; rethrows the exception being handled, caught by its base");

    g_log[0] = 0;
    r = 0;
    try {
        try {
            middle(0);
        } catch (int) {
            lg("caught ");
            middle(2);
        }
    } catch (const Sub &s) {
        r = s.code;
    }
    check(3, r == 22 && !strcmp(g_log, "~t ~m caught ~t ~m "), "a new exception thrown in a catch block goes on out");

    g_log[0] = 0;
    r = 0;
    try {
        r = deep(200);
    } catch (const Err &e) {
        r = e.code;
    }
    int ds = 0;
    for (const char *p = g_log; (p = strstr(p, "~d ")); p += 3) ds++;
    check(4, r == 99 && ds == 201 && g_live == 0, "out of 200 nested frames, each one's destructor run");

    g_log[0] = 0;
    r = 0;
    try {
        with_inner();
    } catch (const Err &e) {
        r = e.code;
    }
    check(5, r == 3 && !strcmp(g_log, "unwinding inner-caught "),
          "a destructor the unwind runs: uncaught_exceptions() 1, a try/catch inside it works");

    g_log[0] = 0;
    r = 0;
    try {
        throw Sub(5);
    } catch (...) {
        try {
            throw;
        } catch (const Err &e) {
            r = e.code + 10 * __uncaught_exceptions();
        }
    }
    check(6, r == 5, "catch (...) and a rethrow inside it, caught by a base");

    DWORD tcode = 0;
    HANDLE th = CreateThread(0, 0, thread_main, 0, 0, 0);
    WaitForSingleObject(th, 20000);
    GetExitCodeThread(th, &tcode);
    CloseHandle(th);
    check(7, tcode == 11, "on another thread");

    char self[MAX_PATH];
    GetModuleFileNameA(0, self, sizeof self);
    char out[256];
    DWORD code = 0x12345;
    int n = run_child(self, "uncaught", out, sizeof out, &code);
    bool ok8 = n >= 0 && !strcmp(out, "child\n") && code == 3;
    printf("     child uncaught exit 0x%08lx %s\n", code, ok8 ? "" : "(not as on Windows)");
    if (!ok8) printf("%s", out);
    check(8, ok8, "an exception nobody catches: terminate, abort, exit code 3");

    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(0);
    wc.lpszClassName = "cxx39";
    RegisterClassA(&wc);
    HWND hw = CreateWindowExA(0, "cxx39", "cxx39", WS_OVERLAPPEDWINDOW, 0, 0, 160, 100, 0, 0, wc.hInstance, 0);
    g_log[0] = 0;
    r = 0;
    try {
        MSG msg;
        memset(&msg, 0, sizeof msg);
        msg.hwnd = hw;
        msg.message = WM_USER;
        DispatchMessageA(&msg);
        lg("not ");
    } catch (const Err &e) {
        r = e.code;
    }
    check(9, hw && r == 77 && !strcmp(g_log, "~w "),
          "a throw out of a window procedure goes through DispatchMessage to the catch around it");

    printf("win-cxx39: %s\n", first_fail ? "a check failed" : "all checks ok");
    return first_fail ? 100 + first_fail : 41;
}
