// win-cxx32 -- Track C, W7a: a C++ program built by the toolchain's g++ the
// ordinary way -- dynamically linked against its own libstdc++-6.dll,
// libgcc_s_seh-1.dll and libwinpthread-1.dll, which live beside it. Nothing
// here throws (exceptions are W7b's). It checks what a C++ program leans on
// the runtime for: static constructors before main and destructors at the
// exit (its own and libstdc++'s: the iostreams), iostream and stringstream
// formatting, strings and the containers, the algorithms, operator new and
// delete, std::function, smart pointers, std::thread with std::mutex and
// std::condition_variable (winpthreads underneath), thread_local, std::chrono --
// and that exit() runs the static destructors after main.
// Measured on Windows 11 first: it exits 32 there. Each check prints ok/FAIL
// with its number; exit 32 iff all pass (the last through the destructor),
// else 100 + the first that failed.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

static int first_fail;
static void check(int n, bool ok, const char *what) {
    std::printf("%s %2d %s\n", ok ? "ok  " : "FAIL", n, what);
    if (!ok && !first_fail) first_fail = n;
}

extern "C" __declspec(dllimport) void *__stdcall GetCurrentProcess(void);
extern "C" __declspec(dllimport) int __stdcall TerminateProcess(void *, unsigned);
static bool g_main_done;

// a static object: constructed before main, destroyed by exit() after main
// returns -- before the DLLs are detached, so std::cout still works. main
// returns 31; this turns it into 32, so the exit code says it ran.
struct Global {
    int value;
    Global() : value(0x5EED) {}
    ~Global() {
        std::cout << "win-cxx32: the static destructor ran, at the exit" << std::endl;
        if (g_main_done && !first_fail) TerminateProcess(GetCurrentProcess(), 32);
    }
};
static Global g_global;

static thread_local int t_counter = 100;

struct Shape {
    virtual ~Shape() {}
    virtual int area() const = 0;
};
struct Rect : Shape {
    int w, h;
    Rect(int a, int b) : w(a), h(b) {}
    int area() const override { return w * h; }
};

int main() {
    std::cout << "win-cxx32: C++ on the toolchain's own DLLs" << std::endl;

    check(1, g_global.value == 0x5EED, "a static object was constructed before main");

    std::ostringstream os;
    os << "n=" << 42 << " hex=" << std::hex << 255 << std::dec << " f=" << 1.5;
    check(2, os.str() == "n=42 hex=ff f=1.5", "ostringstream: integers, std::hex, a double");

    std::istringstream is("17 x 3.25");
    int a = 0; std::string word; double d = 0;
    is >> a >> word >> d;
    check(3, a == 17 && word == "x" && d == 3.25, "istringstream parses an int, a word, a double");

    std::string s = "hello";
    s += ", world";
    s.insert(0, ">> ");
    check(4, s == ">> hello, world" && s.find("world") == 10 && s.substr(3, 5) == "hello",
          "std::string: append, insert, find, substr");

    std::vector<int> v;
    for (int i = 0; i < 1000; i++) v.push_back((i * 7919) % 1000);
    std::sort(v.begin(), v.end());
    bool sorted = std::is_sorted(v.begin(), v.end()) && v.front() == 0 && v.back() == 999;
    check(5, sorted && std::accumulate(v.begin(), v.end(), 0L) == 499500L,
          "std::vector of 1000, std::sort, std::accumulate");

    std::map<std::string, int> m;
    std::unordered_map<int, std::string> um;
    for (int i = 0; i < 100; i++) {
        m["k" + std::to_string(i)] = i;
        um[i] = std::to_string(i * i);
    }
    check(6, m.size() == 100 && m["k42"] == 42 && m.begin()->first == "k0" && um[12] == "144",
          "std::map and std::unordered_map, std::to_string");

    std::unique_ptr<Shape> up(new Rect(3, 4));
    std::shared_ptr<Shape> sp = std::make_shared<Rect>(5, 6);
    std::shared_ptr<Shape> sp2 = sp;
    check(7, up->area() == 12 && sp->area() == 30 && sp.use_count() == 2,
          "unique_ptr, shared_ptr (use_count 2), a virtual call");

    std::function<int(int)> twice = [](int x) { return 2 * x; };
    int captured = 10;
    std::function<int(int)> plus = [captured](int x) { return x + captured; };
    check(8, twice(21) == 42 && plus(5) == 15, "std::function holding lambdas, one capturing");

    // threads: a mutex-guarded counter, a condition variable handshake
    std::mutex mu;
    std::condition_variable cv;
    long total = 0;
    int ready = 0;
    std::vector<std::thread> pool;
    for (int t = 0; t < 4; t++)
        pool.emplace_back([&, t] {
            for (int i = 0; i < 10000; i++) {
                std::lock_guard<std::mutex> lk(mu);
                total += t + 1;
            }
            std::lock_guard<std::mutex> lk(mu);
            ready++;
            cv.notify_one();
        });
    {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return ready == 4; });
    }
    for (auto &th : pool) th.join();
    check(9, total == 10000L * (1 + 2 + 3 + 4) && ready == 4,
          "4 std::threads, a std::mutex, a std::condition_variable");

    // thread_local: a copy per thread
    t_counter = 7;
    int seen = -1;
    std::thread tt([&] { seen = t_counter; t_counter = 99; });
    tt.join();
    check(10, seen == 100 && t_counter == 7, "thread_local: each thread its own copy");

    std::atomic<int> at(0);
    std::vector<std::thread> pool2;
    for (int t = 0; t < 3; t++) pool2.emplace_back([&] { for (int i = 0; i < 5000; i++) at++; });
    for (auto &th : pool2) th.join();
    check(11, at.load() == 15000, "std::atomic incremented by 3 threads");

    auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    check(12, ms >= 25 && ms < 5000, "std::chrono::steady_clock and sleep_for(30 ms)");

    std::cout << "win-cxx32: " << (first_fail ? "a check failed" : "checks 1-12 ok; main returns 31, the "
                 "static destructor makes it 32") << std::endl;
    g_main_done = true;
    return first_fail ? 100 + first_fail : 31;
}
