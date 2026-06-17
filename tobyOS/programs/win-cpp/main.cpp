/* main.cpp -- a STOCK C++ Windows .exe (Track C / C4). Built with a plain
 * `clang++ main.cpp -o win-cpp.exe` (no flags, no custom entry). Beyond the C
 * CRT startup proven in C3, this exercises C++ GLOBAL CONSTRUCTORS: the
 * mingw runtime calls them from static `__main` -> `__do_global_ctors` (which
 * walks `__CTOR_LIST__`) in CPL3 user code, so g_greeter's constructor runs
 * before main(). main checks the constructor's side-effect and returns 5 iff
 * it ran. It also pulls in the wider ucrt surface a C++ program needs
 * (_errno, localeconv, stdio FILE locking, wide/multibyte string helpers),
 * which tobyOS shims.
 */

#include <cstdio>

struct Greeter {
    int v;
    Greeter() { v = 1234; printf("[ctor] global constructor ran, v=%d\n", v); }
};

static Greeter g_greeter;   /* its constructor must run before main() */

int main() {
    printf("[main] g_greeter.v = %d (expect 1234 iff the ctor ran)\n", g_greeter.v);
    return (g_greeter.v == 1234) ? 5 : 1;
}
