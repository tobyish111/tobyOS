// win-cxx42 -- Track C, W7k: what W7h left of Microsoft's C++ runtime. Built
// by cl.exe (/EHsc /MD) against Microsoft's redistributable msvcp140.dll (the
// STL's DLL part: iostreams, threads, mutexes, condition variables, the
// clocks) as well as vcruntime140.dll and vcruntime140_1.dll. The invalid-
// parameter handler a program sets is called; a thread_local object's
// destructor runs when its thread ends (and the main thread's at exit);
// set_terminate's handler is what an uncaught exception reaches. Each check
// what Windows 11 does (measured first: it exits 44 there). Exit 44 iff all
// pass, else 100 + the first that failed.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <sstream>
#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <atomic>

static int first_fail;
static void check(int n, bool ok, const char *what) {
    printf("%s %2d %s\n", ok ? "ok  " : "FAIL", n, what);
    if (!ok && !first_fail) first_fail = n;
}
static void say(const char *s) {
    DWORD n;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, (DWORD)strlen(s), &n, 0);
}

// ---- 1: the invalid-parameter handler ----
static int g_iph;
static const wchar_t *g_iph_expr, *g_iph_func, *g_iph_file;
static unsigned g_iph_line;
static uintptr_t g_iph_res;
static void __cdecl iph(const wchar_t *expr, const wchar_t *func, const wchar_t *file, unsigned line, uintptr_t res) {
    g_iph++;
    g_iph_expr = expr; g_iph_func = func; g_iph_file = file; g_iph_line = line; g_iph_res = res;
}

// ---- 2: thread_local destructors ----
static std::atomic<int> g_tl_ctor, g_tl_dtor;
static std::atomic<DWORD> g_tl_dtor_tid;
struct TL {
    int v;
    TL() : v(7) { g_tl_ctor++; }
    ~TL() { g_tl_dtor++; g_tl_dtor_tid = GetCurrentThreadId(); if (g_child_say) say("~TL\n"); }
    static bool g_child_say;
};
bool TL::g_child_say;
static thread_local TL t_obj;
struct Static { ~Static() { if (TL::g_child_say) say("~Static\n"); } };

// ---- children ----
static void on_terminate() {
    say("terminate handler\n");
    ExitProcess(5);
}
static int child(const char *m) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (!strcmp(m, "term")) {
        (void)std::set_terminate(on_terminate);          /* (the runtime's own came before) */
        if (std::get_terminate() != on_terminate) say("get_terminate wrong\n");
        throw std::runtime_error("nobody catches this");
    }
    if (!strcmp(m, "tlexit")) {                     // the main thread's thread_local, at exit
        TL::g_child_say = true;
        static Static s;
        say(t_obj.v == 7 ? "main uses t_obj\n" : "t_obj wrong\n");
        return 3;
    }
    if (!strcmp(m, "cout")) {
        std::cout << "cout " << 42 << ' ' << 2.5 << std::endl;
        std::cerr << "cerr line" << std::endl;
        return 4;
    }
    return 9;
}
static DWORD run_self(const char *self, const char *arg, char *out, int cap) {
    char cmd[600];
    snprintf(cmd, sizeof cmd, "\"%s\" %s", self, arg);
    SECURITY_ATTRIBUTES sa = { sizeof sa, 0, TRUE };
    HANDLE r, w;
    if (!CreatePipe(&r, &w, &sa, 0)) return 0xDEAD;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = w;
    si.hStdError = w;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    if (!CreateProcessA(0, cmd, 0, 0, TRUE, 0, 0, 0, &si, &pi)) return 0xDEAD;
    CloseHandle(w);
    int n = 0;
    DWORD got;
    while (n < cap - 1 && ReadFile(r, out + n, (DWORD)(cap - 1 - n), &got, 0) && got) n += (int)got;
    out[n] = 0;
    CloseHandle(r);
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD ec = 0xBEEF;
    GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ec;
}

int main(int argc, char **argv) {
    if (argc > 1) return child(argv[1]);
    char out[1024];

    // 1
    {
        _invalid_parameter_handler prev = _set_invalid_parameter_handler(iph);
        char buf[4] = "abc";
        errno = 0;
        errno_t e = strcpy_s(buf, sizeof buf, "toolong");
        bool ok = !prev && g_iph == 1 && e == ERANGE && errno == ERANGE && buf[0] == 0 &&
                  !g_iph_expr && !g_iph_func && !g_iph_file && g_iph_line == 0 && g_iph_res == 0;
        strcpy_s(buf, sizeof buf, "ab");
        e = strcat_s(buf, sizeof buf, "cd");
        ok = ok && g_iph == 2 && e == ERANGE && buf[0] == 0;
        e = strcpy_s(nullptr, 4, "x");
        ok = ok && g_iph == 3 && e == EINVAL;
        ok = ok && _get_invalid_parameter_handler() == iph && _set_invalid_parameter_handler(nullptr) == iph;
        printf("    handler calls %d\n", g_iph);
        check(1, ok, "the invalid-parameter handler set is called (all its arguments empty), the function returns ERANGE / EINVAL");
    }
    // 2
    {
        int c0 = g_tl_ctor, d0 = g_tl_dtor;
        DWORD tid = 0;
        int seen = 0;
        std::thread t([&] { tid = GetCurrentThreadId(); seen = t_obj.v; });
        t.join();
        bool ok = seen == 7 && g_tl_ctor == c0 + 1 && g_tl_dtor == d0 + 1 && g_tl_dtor_tid == tid;
        DWORD ec = run_self(argv[0], "tlexit", out, sizeof out);
        printf("    child tlexit: %lu [%s]\n", ec, out);
        ok = ok && ec == 3 && !strcmp(out, "main uses t_obj\n~TL\n~Static\n");
        check(2, ok, "a thread_local's destructor runs as its thread ends, on that thread; the main thread's at exit, before the statics'");
    }
    // 3
    {
        DWORD ec = run_self(argv[0], "term", out, sizeof out);
        printf("    child term: %lu [%s]\n", ec, out);
        check(3, ec == 5 && !strcmp(out, "terminate handler\n"), "set_terminate: an uncaught exception reaches the handler (get_terminate says so)");
    }
    // 4: iostreams
    {
        std::ostringstream os;
        os << "a=" << 42 << " b=" << 3.5 << " c=" << std::hex << 255;
        std::istringstream is("17 2.25 word");
        int i = 0; double d = 0; std::string w;
        is >> i >> d >> w;
        DWORD ec = run_self(argv[0], "cout", out, sizeof out);
        /* the child's lines without their '\r's: Windows' text-mode stdio
         * writes "\r\n" for '\n', tobyOS's does not (its stdio is binary) */
        int k = 0;
        for (int j = 0; out[j]; j++) if (out[j] != '\r') out[k++] = out[j];
        out[k] = 0;
        printf("    [%s] %d %g [%s]; child cout: %lu [%s]\n", os.str().c_str(), i, d, w.c_str(), ec, out);
        check(4, os.str() == "a=42 b=3.5 c=ff" && i == 17 && d == 2.25 && w == "word" && ec == 4 &&
                     !strcmp(out, "cout 42 2.5\ncerr line\n"),
              "msvcp140: ostringstream, istringstream, std::cout and std::cerr");
    }
    // 5: strings and containers
    {
        std::string s = "hello";
        s += ", world";
        std::vector<int> v = { 5, 3, 9, 1 };
        std::sort(v.begin(), v.end());
        std::map<std::string, int> m;
        m["b"] = 2; m["a"] = 1;
        bool caught = false;
        try { (void)v.at(10); } catch (const std::out_of_range &e) { caught = strstr(e.what(), "subscript") != nullptr; }
        check(5, s.find("world") == 7 && s.substr(0, 5) == "hello" && std::to_string(1234) == "1234" &&
                     v[0] == 1 && v[3] == 9 && m.begin()->first == "a" && caught,
              "std::string, std::to_string, sort, map; vector::at throws out_of_range");
    }
    // 6: threads, a mutex, a condition variable
    {
        std::mutex mu;
        std::condition_variable cv;
        std::vector<int> q;
        bool done = false;
        long long sum = 0;
        std::thread consumer([&] {
            std::unique_lock<std::mutex> lk(mu);
            for (;;) {
                cv.wait(lk, [&] { return !q.empty() || done; });
                while (!q.empty()) { sum += q.back(); q.pop_back(); }
                if (done) break;
            }
        });
        std::vector<std::thread> producers;
        for (int p = 0; p < 4; p++)
            producers.emplace_back([&, p] {
                for (int k = 1; k <= 250; k++) {
                    std::lock_guard<std::mutex> g(mu);
                    q.push_back(p * 1000 + k);
                    cv.notify_one();
                }
            });
        for (auto &t : producers) t.join();
        { std::lock_guard<std::mutex> g(mu); done = true; }
        cv.notify_all();
        consumer.join();
        long long want = 0;
        for (int p = 0; p < 4; p++) for (int k = 1; k <= 250; k++) want += p * 1000 + k;
        printf("    sum %lld (want %lld), hardware_concurrency %u\n", sum, want, std::thread::hardware_concurrency());
        check(6, sum == want, "std::thread, std::mutex, std::condition_variable: four producers, one consumer");
    }
    // 7: the clocks
    {
        auto a = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        auto b = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
        auto sys = std::chrono::system_clock::now().time_since_epoch();
        long long secs = std::chrono::duration_cast<std::chrono::seconds>(sys).count();
        printf("    slept %lld ms; system_clock %lld s\n", (long long)ms, secs);
        check(7, ms >= 25 && ms < 2000 && secs > 1700000000LL, "steady_clock across sleep_for(30 ms); system_clock is the time of day");
    }
    printf("%s\n", first_fail ? "SOME FAILED" : "ALL PASS");
    return first_fail ? 100 + first_fail : 44;
}
