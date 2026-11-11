// win-exc33 -- Track C, W7b: C++ exceptions. Built by the toolchain's g++
// the ordinary way, on its own libstdc++-6.dll / libgcc_s_seh-1.dll /
// libwinpthread-1.dll (beside it), as win-cxx32 is. A throw on x64 Windows
// is SEH: libgcc's _Unwind_RaiseException calls RaiseException, the
// system's dispatcher walks the frames (RtlLookupFunctionEntry,
// RtlVirtualUnwind) calling each one's language handler, and the handler
// that takes it unwinds to the catch with RtlUnwindEx, which runs the
// cleanups (destructors) of every frame in between. The checks: a throw
// caught in its own function, three frames up with the destructors of the
// frames between run in order, one thrown inside libstdc++-6.dll
// (vector::at, stoi) caught here, catch by a base class, rethrow, nested try
// blocks, catch (...), a thousand throws, a throw and catch on another
// thread, and an exception_ptr carried from one thread to another.
// Measured on Windows 11 first: it exits 33 there. Each check prints ok/FAIL
// with its number; exit 33 iff all pass, else 100 + the first that failed.
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static int first_fail;
static void check(int n, bool ok, const char *what) {
    std::printf("%s %2d %s\n", ok ? "ok  " : "FAIL", n, what);
    if (!ok && !first_fail) first_fail = n;
}

static std::string g_trace;
struct Guard {
    char tag;
    explicit Guard(char t) : tag(t) {}
    ~Guard() { g_trace += tag; }
};

struct MyError {
    int code;
};

__attribute__((noinline)) static void level3(int v) {
    Guard g('3');
    if (v > 0) throw std::runtime_error("deep " + std::to_string(v));
}
__attribute__((noinline)) static void level2(int v) {
    Guard g('2');
    level3(v);
    g_trace += 'x';                      // never reached
}
__attribute__((noinline)) static void level1(int v) {
    Guard g('1');
    level2(v);
    g_trace += 'y';
}

__attribute__((noinline)) static int thrower(int i) {
    if (i % 3 == 0) throw i;
    if (i % 3 == 1) throw std::string("s");
    throw MyError{i};
}

int main() {
    std::printf("win-exc33: C++ exceptions on the toolchain's own DLLs\n");

    int got = 0;
    try { throw 41; } catch (int v) { got = v + 1; }
    check(1, got == 42, "throw 41 caught in the same function");

    g_trace.clear();
    std::string what;
    try {
        Guard outer('0');
        level1(7);
    } catch (const std::runtime_error &e) {
        what = e.what();
        g_trace += 'c';
    }
    check(2, what == "deep 7" && g_trace == "3210c",
          "a runtime_error from three frames up; their destructors ran in order, then the catch");
    std::printf("     (trace %s)\n", g_trace.c_str());

    bool caught_at = false;
    try {
        std::vector<int> v(3);
        (void)v.at(10);
    } catch (const std::out_of_range &) { caught_at = true; }
    check(3, caught_at, "vector::at past the end: std::out_of_range thrown inside libstdc++-6.dll, caught here");

    bool caught_stoi = false;
    try { (void)std::stoi("not a number"); } catch (const std::invalid_argument &) { caught_stoi = true; }
    check(4, caught_stoi, "std::stoi(\"not a number\"): std::invalid_argument");

    std::string base_what;
    try { throw std::logic_error("by base"); } catch (const std::exception &e) { base_what = e.what(); }
    check(5, base_what == "by base", "a logic_error caught as std::exception&, what() intact");

    int rethrown = 0;
    try {
        try { throw MyError{9}; } catch (MyError &e) { e.code++; throw; }
    } catch (const MyError &e) { rethrown = e.code; }
    check(6, rethrown == 10, "throw; from a catch rethrows the same object (changed in the first catch)");

    int inner = 0, outer2 = 0;
    try {
        try { throw std::string("str"); } catch (int) { inner = 1; }
    } catch (const std::string &s) { outer2 = (s == "str") ? 2 : 0; }
    check(7, inner == 0 && outer2 == 2, "nested try: the inner catch does not match, the outer one does");

    bool any = false;
    try { throw MyError{1}; } catch (...) { any = true; }
    check(8, any, "catch (...) takes a type of the program's own");

    int ints = 0, strs = 0, mine = 0;
    for (int i = 0; i < 1000; i++) {
        try { thrower(i); }
        catch (int) { ints++; }
        catch (const std::string &) { strs++; }
        catch (const MyError &) { mine++; }
    }
    check(9, ints == 334 && strs == 333 && mine == 333, "a thousand throws of three types, each caught by its own catch");

    int in_thread = 0;
    std::thread t([&] {
        try { level1(3); } catch (const std::exception &e) { in_thread = std::string(e.what()) == "deep 3" ? 1 : 2; }
    });
    t.join();
    check(10, in_thread == 1, "a throw and catch on another thread");

    std::exception_ptr ep;
    std::thread t2([&] {
        try { throw std::runtime_error("carried"); } catch (...) { ep = std::current_exception(); }
    });
    t2.join();
    std::string carried;
    try { if (ep) std::rethrow_exception(ep); } catch (const std::runtime_error &e) { carried = e.what(); }
    check(11, carried == "carried", "an exception_ptr from another thread, rethrown and caught here");

    std::printf("win-exc33: %s\n", first_fail ? "a check failed" : "all checks ok");
    return first_fail ? 100 + first_fail : 33;
}
