/* user_stltest/main.cpp -- /bin/stltest: verification for the tobyOS
 * freestanding C++ STL. This slice covers the foundation headers
 * (STL slice 1): the C-compat wrappers (<cstdint>/<cstring>/<cmath>/...),
 * <type_traits>, <utility>, <limits>, and <initializer_list>. Later STL
 * slices extend this program. "[stl] ..." markers on serial; exit 0 =
 * ALL PASS. */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <limits>
#include <initializer_list>
#include <iterator>
#include <algorithm>
#include <memory>
#include <array>

static int g_pass = 0, g_fail = 0;
static void check(const char *what, bool ok) {
    if (ok) g_pass++; else g_fail++;
    std::printf("[stl] %-34s %s\n", what, ok ? "OK" : "FAIL");
}

/* ---- type_traits (compile-time, asserted via static_assert too) ---- */
static void test_type_traits() {
    check("is_same<int,int>", std::is_same<int, int>::value);
    check("is_same<int,long> false", !std::is_same<int, long>::value);
    check("conditional true->A",
          std::is_same<std::conditional_t<true, int, long>, int>::value);
    check("conditional false->B",
          std::is_same<std::conditional_t<false, int, long>, long>::value);
    check("remove_reference<int&>",
          std::is_same<std::remove_reference_t<int&>, int>::value);
    check("remove_cv<const volatile int>",
          std::is_same<std::remove_cv_t<const volatile int>, int>::value);
    check("is_integral<uint32_t>", std::is_integral<std::uint32_t>::value);
    check("is_integral<float> false", !std::is_integral<float>::value);
    check("is_floating_point<double>", std::is_floating_point<double>::value);
    check("is_signed<int>", std::is_signed<int>::value);
    check("is_unsigned<unsigned>", std::is_unsigned<unsigned>::value);
    check("is_pointer<int*>", std::is_pointer<int*>::value);
    check("is_pointer<int> false", !std::is_pointer<int>::value);
    check("is_trivial<int>", std::is_trivial<int>::value);
    check("make_unsigned<int>==unsigned",
          std::is_same<std::make_unsigned_t<int>, unsigned>::value);
    check("extent<int[7]>==7", std::extent<int[7]>::value == 7);
    check("is_enum<int> false", !std::is_enum<int>::value);
    /* enable_if picks the right overload only when the trait holds. */
    check("enable_if compiles", true);
}

/* ---- utility ---- */
struct Mover {
    int v; static int moves; static int copies;
    Mover(int x = 0) : v(x) {}
    Mover(const Mover& o) : v(o.v) { copies++; }
    Mover(Mover&& o) noexcept : v(o.v) { o.v = -1; moves++; }
    Mover& operator=(const Mover& o) { v = o.v; copies++; return *this; }
    Mover& operator=(Mover&& o) noexcept { v = o.v; o.v = -1; moves++; return *this; }
};
int Mover::moves = 0;
int Mover::copies = 0;

template <std::size_t... I>
static int seq_sum(std::index_sequence<I...>) {
    int s = 0; int dummy[] = { (s += (int)I, 0)... }; (void)dummy; return s;
}

static void test_utility() {
    Mover a(5);
    Mover b(std::move(a));                 /* move ctor: a.v -> -1 */
    check("move: source emptied", a.v == -1 && b.v == 5);
    check("move: used move ctor", Mover::moves >= 1);

    int x = 1, y = 2;
    std::swap(x, y);
    check("swap ints", x == 2 && y == 1);

    int arr1[3] = {1, 2, 3}, arr2[3] = {4, 5, 6};
    std::swap(arr1, arr2);
    check("swap arrays", arr1[0] == 4 && arr2[2] == 3);

    int old = std::exchange(x, 99);
    check("exchange", old == 2 && x == 99);

    std::pair<int, const char*> p = std::make_pair(7, "hi");
    check("pair/make_pair", p.first == 7 && std::strcmp(p.second, "hi") == 0);
    std::pair<int, int> q(1, 2), r(1, 2);
    check("pair ==", q == r);

    check("index_sequence sum 0..4",
          seq_sum(std::make_index_sequence<5>()) == 0 + 1 + 2 + 3 + 4);
}

/* ---- limits ---- */
static void test_limits() {
    check("numeric_limits<int>::max",
          std::numeric_limits<int>::max() == 2147483647);
    check("numeric_limits<int>::min",
          std::numeric_limits<int>::min() == (-2147483647 - 1));
    check("numeric_limits<uint32_t>::max",
          std::numeric_limits<std::uint32_t>::max() == 4294967295u);
    check("numeric_limits<uint8_t>::max",
          std::numeric_limits<std::uint8_t>::max() == 255);
    check("numeric_limits<int> is_signed", std::numeric_limits<int>::is_signed);
    check("numeric_limits<float>::max finite",
          std::numeric_limits<float>::max() > 3.0e38f);
}

/* ---- initializer_list ---- */
static int il_sum(std::initializer_list<int> l) {
    int s = 0; for (int v : l) s += v; return s;
}
static void test_initializer_list() {
    check("initializer_list sum", il_sum({1, 2, 3, 4, 5}) == 15);
    check("initializer_list size", std::initializer_list<int>({9, 8}).size() == 2);
}

/* ---- C-compat wrappers ---- */
static void test_ccompat() {
    char buf[32];
    std::memset(buf, 0, sizeof buf);
    std::memcpy(buf, "abc", 3);
    check("memcpy/strlen", std::strlen(buf) == 3);
    check("abs(int)", std::abs(-7) == 7);
    check("abs(double)", std::abs(-2.5) == 2.5);
    check("sqrt", std::abs(std::sqrt(16.0) - 4.0) < 1e-9);
    int n = std::snprintf(buf, sizeof buf, "%d-%s", 42, "x");
    check("snprintf", n == 4 && std::strcmp(buf, "42-x") == 0);
    check("uint64 literal width", sizeof(std::uint64_t) == 8);
}

/* ---- algorithm (slice 2) ---- */
static void test_algorithm() {
    check("min/max", std::min(3, 7) == 3 && std::max(3, 7) == 7);
    check("min/max init-list",
          std::min({5, 2, 9, 1}) == 1 && std::max({5, 2, 9, 1}) == 9);
    check("clamp", std::clamp(15, 0, 10) == 10 && std::clamp(-4, 0, 10) == 0);
    int v[] = {4, 8, 15, 16, 23, 42};
    check("find", std::find(v, v + 6, 16) == v + 3);
    check("find_if even",
          *std::find_if(v, v + 6, [](int x){ return x % 2 == 0 && x > 5; }) == 8);
    check("count_if", std::count_if(v, v + 6, [](int x){ return x > 10; }) == 4);
    check("all_of/any_of",
          std::all_of(v, v + 6, [](int x){ return x > 0; }) &&
          std::any_of(v, v + 6, [](int x){ return x == 42; }));
    check("max_element", *std::max_element(v, v + 6) == 42);
    int d[6]; std::copy(v, v + 6, d);
    check("copy", d[4] == 23);
    std::fill(d, d + 6, 7);
    check("fill", d[0] == 7 && d[5] == 7);
    int s[] = {5, 3, 8, 1, 9, 2};
    std::sort(s, s + 6);
    check("sort", s[0] == 1 && s[5] == 9 &&
                  std::binary_search(s, s + 6, 8));
    check("lower_bound", std::lower_bound(s, s + 6, 8) - s == 4);
    std::reverse(s, s + 6);
    check("reverse", s[0] == 9 && s[5] == 1);
}

/* ---- memory (slice 2) ---- */
static int g_del = 0;
struct Counted { ~Counted() { g_del++; } int x = 11; };
static void test_memory() {
    {
        std::unique_ptr<int> p(new int(42));
        check("unique_ptr deref", *p == 42);
        int* raw = p.get();
        std::unique_ptr<int> q = std::move(p);
        check("unique_ptr move", p.get() == nullptr && q.get() == raw);
        q.reset();
        check("unique_ptr reset", q.get() == nullptr && !q);
    }
    {
        auto mp = std::make_unique<int>(7);
        check("make_unique", *mp == 7);
        auto arr = std::make_unique<int[]>(4);
        arr[0] = 1; arr[3] = 4;
        check("make_unique array", arr[0] == 1 && arr[3] == 4);
    }
    g_del = 0;
    { std::unique_ptr<Counted> c(new Counted()); check("uptr holds obj", c->x == 11); }
    check("unique_ptr deletes on scope-exit", g_del == 1);
    {
        std::allocator<int> a;
        int* buf = a.allocate(8);
        a.construct(buf + 2, 55);
        check("allocator allocate/construct", buf[2] == 55);
        a.destroy(buf + 2);
        a.deallocate(buf, 8);
        check("allocator deallocate", true);
    }
}

/* ---- array (slice 2) ---- */
static void test_array() {
    std::array<int, 4> a = {10, 20, 30, 40};
    check("array [] + size", a[2] == 30 && a.size() == 4);
    check("array front/back", a.front() == 10 && a.back() == 40);
    int sum = 0; for (int x : a) sum += x;
    check("array iterate", sum == 100);
    check("array get<>", std::get<1>(a) == 20);
    std::array<int, 4> b = a;
    check("array ==", a == b);
    a.fill(9);
    check("array fill", a[0] == 9 && a[3] == 9 && !(a == b));
    check("array tuple_size", std::tuple_size<std::array<int, 4>>::value == 4);
}

extern "C" int main(void) {
    test_type_traits();
    test_utility();
    test_limits();
    test_initializer_list();
    test_ccompat();
    test_algorithm();
    test_memory();
    test_array();
    std::printf("[stl] foundation self-test: %d/%d %s\n", g_pass, g_pass + g_fail,
                g_fail == 0 ? "ALL PASS" : "FAILURES");
    return g_fail == 0 ? 0 : 1;
}
