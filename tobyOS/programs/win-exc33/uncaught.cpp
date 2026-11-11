// win-unc33 -- Track C, W7b: a C++ exception nobody catches. The system's
// dispatcher finds no handler; it calls the unhandled-exception filter the
// mingw C runtime installed at its start (SetUnhandledExceptionFilter),
// which lets a GCC exception go on, so RaiseException returns to libgcc,
// and libstdc++ calls std::terminate -- here our own terminate handler,
// which ends the process with 34 after checking the destructor of the
// throwing frame did NOT run (no handler, so no unwinding: the C++ standard
// leaves that to the implementation; GCC does not unwind). Measured on
// Windows 11 first: it exits 34 there.
#include <cstdio>
#include <exception>
#include <stdexcept>

extern "C" __declspec(dllimport) void *__stdcall GetCurrentProcess(void);
extern "C" __declspec(dllimport) int __stdcall TerminateProcess(void *, unsigned);

static bool g_unwound;
struct Guard {
    ~Guard() { g_unwound = true; }
};

static void on_terminate() {
    std::printf("win-unc33: std::terminate, the frame %s\n", g_unwound ? "WAS unwound" : "was not unwound");
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), g_unwound ? 135 : 34);
}

__attribute__((noinline)) static void boom() {
    Guard g;
    throw std::runtime_error("nobody catches this");
}

int main() {
    std::set_terminate(on_terminate);
    std::printf("win-unc33: throwing with no catch\n");
    std::fflush(stdout);
    boom();
    std::printf("win-unc33: FAIL -- boom() returned\n");
    return 136;
}
