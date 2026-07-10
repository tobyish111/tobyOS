/* user_cpptest/main.cpp -- /bin/cpptest: the C++ runtime acceptance
 * test (stage 13 C++ runtime). REAL C++ compiled by clang++ with
 * -fno-exceptions -fno-rtti against libtoby: global constructors
 * (.init_array via program.ld + the init.c walk), virtual dispatch
 * through vtables, templates, new/new[]/delete, placement new,
 * function-local statics (__cxa_guard_*), and a static destructor
 * (__cxa_atexit) that proves itself on exit.
 *
 * Results print as "[cpp] ..." serial markers AND render in a TobyTK
 * table, so the QMU harness verifies both. 'q' quits (the static
 * destructor marker then fires on the way out).
 */

#include <toby/tk.h>
#include <stdio.h>
#include <string.h>
#include <new>

/* ---- 1. global constructors ---------------------------------------- */

static int g_ctor_calls;

struct GlobalProbe {
    int value;
    explicit GlobalProbe(int v) : value(v) { g_ctor_calls++; }
};

static GlobalProbe g_probe_a(41);
static GlobalProbe g_probe_b(1);

/* ---- 2. static destructor (__cxa_atexit) --------------------------- */

struct ExitProbe {
    ~ExitProbe() {
        /* Runs after tk_run returns, on the exit() path. */
        printf("[cpp] static destructor ran (cxa_atexit) OK\n");
    }
};

static ExitProbe g_exit_probe;

/* ---- 3. virtual dispatch ------------------------------------------- */

struct Shape {
    virtual ~Shape() {}
    virtual int area() const = 0;
    virtual const char *name() const { return "shape"; }
};

struct Circle : Shape {
    int r;
    explicit Circle(int rr) : r(rr) {}
    int area() const override { return 314 * r * r / 100; }
    const char *name() const override { return "circle"; }
};

struct Rect : Shape {
    int w, h;
    Rect(int ww, int hh) : w(ww), h(hh) {}
    int area() const override { return w * h; }
    const char *name() const override { return "rect"; }
};

/* ---- 4. templates --------------------------------------------------- */

template <typename T> static T tmax(T a, T b) { return a > b ? a : b; }

template <typename T, int N> struct Stack {
    T items[N];
    int n = 0;
    bool push(T v) { if (n >= N) return false; items[n++] = v; return true; }
    T pop() { return n > 0 ? items[--n] : T(); }
    int size() const { return n; }
};

/* ---- 5. function-local static (__cxa_guard_*) ----------------------- */

static int g_local_static_ctors;

struct Counter {
    int n;
    Counter() : n(100) { g_local_static_ctors++; }
};

static int bump_local_static(void) {
    static Counter c;              /* must construct exactly once */
    return ++c.n;
}

/* ---- test harness ---------------------------------------------------- */

#define NTESTS 7
static char g_rows[NTESTS][2][64];
static const char *const tbl_headers[] = { "C++ feature", "Result" };
static const int          tbl_widths[] = { 300, 120 };
static int g_pass;

static const char *row_cell(void *u, int r, int c) {
    (void)u;
    if (r < 0 || r >= NTESTS || c < 0 || c > 1) return "";
    return g_rows[r][c];
}

static void record(int i, const char *what, bool ok) {
    strcpy(g_rows[i][0], what);
    strcpy(g_rows[i][1], ok ? "PASS" : "FAIL");
    if (ok) g_pass++;
    printf("[cpp] %-34s %s\n", what, ok ? "OK" : "FAIL");
}

static struct tk_window win;

static void on_key(struct tk_window *w, struct tk_event *ev) {
    if (ev->key == 'q' || ev->key == 'Q' || ev->key == 27) tk_quit(w);
}

/* Freestanding C++ would mangle main; crt0 links the C symbol. */
extern "C" int main(int argc, char **argv);

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* 1. global constructors ran before main, in declaration order */
    record(0, "global constructors (.init_array)",
           g_ctor_calls == 2 && g_probe_a.value == 41 &&
           g_probe_b.value == 1);

    /* 2. virtual dispatch through base pointers from operator new */
    Shape *shapes[2];
    shapes[0] = new Circle(10);
    shapes[1] = new Rect(6, 7);
    int area_sum = 0;
    bool names_ok = strcmp(shapes[0]->name(), "circle") == 0 &&
                    strcmp(shapes[1]->name(), "rect") == 0;
    for (int i = 0; i < 2; i++) area_sum += shapes[i]->area();
    record(1, "virtual dispatch + new/delete",
           area_sum == 314 + 42 && names_ok);
    delete shapes[0];
    delete shapes[1];

    /* 3. new[] / delete[] */
    int *arr = new int[100];
    for (int i = 0; i < 100; i++) arr[i] = i;
    int sum = 0;
    for (int i = 0; i < 100; i++) sum += arr[i];
    delete[] arr;
    record(2, "operator new[] / delete[]", sum == 4950);

    /* 4. templates (function + class) */
    Stack<int, 8> st;
    st.push(3); st.push(1); st.push(4);
    bool tpl_ok = tmax(21, 42) == 42 && tmax('a', 'z') == 'z' &&
                  st.size() == 3 && st.pop() == 4;
    record(3, "templates (function + class)", tpl_ok);

    /* 5. function-local static: guard must run the ctor exactly once */
    int v1 = bump_local_static();
    int v2 = bump_local_static();
    int v3 = bump_local_static();
    record(4, "local static (__cxa_guard)",
           g_local_static_ctors == 1 && v1 == 101 && v2 == 102 && v3 == 103);

    /* 6. placement new + explicit destructor call */
    alignas(Circle) unsigned char buf[sizeof(Circle)];
    Circle *pc = new (buf) Circle(2);
    bool placed_ok = pc->area() == 12 && (void *)pc == (void *)buf;
    pc->~Circle();
    record(5, "placement new", placed_ok);

    /* 7. nothrow new */
    int *pn = new (std::nothrow) int(7);
    record(6, "nothrow new", pn != nullptr && *pn == 7);
    delete pn;

    printf("[cpp] %s %d/%d\n", g_pass == NTESTS ? "ALL PASS" : "FAILURES",
           g_pass, NTESTS);
    fflush(stdout);

    /* ---- TobyTK window with the results table ---- */
    if (tk_window_open(&win, 480, 380, "C++ Runtime Test") != 0)
        return g_pass == NTESTS ? 0 : 1;
    tk_on_key(&win, on_key);

    struct tk_widget *root = tk_root(&win);
    tk_pad(root, 16);
    root->gap = 6;

    tk_bold(tk_font(tk_label(&win, root, "tobyOS C++ runtime"), 18));
    tk_colors(tk_label(&win, root,
                       "clang++ -fno-exceptions -fno-rtti + libtoby cxxrt"),
              0, 0x00808890);
    tk_separator(&win, root);
    {
        struct tk_widget *tbl = tk_table(&win, root, tbl_headers,
                                         tbl_widths, 2);
        tk_table_rows(&win, tbl, NTESTS, row_cell, 0);
        tk_grow(tbl, 1);
    }
    tk_separator(&win, root);
    {
        char msg[48];
        snprintf(msg, sizeof(msg), "%d / %d passed  --  Q to quit",
                 g_pass, NTESTS);
        tk_align(tk_colors(tk_label(&win, root, msg), 0,
                           g_pass == NTESTS ? 0x0066BB6A : 0x00EF5350),
                 TK_ALIGN_CENTER);
    }

    tk_run(&win);
    return g_pass == NTESTS ? 0 : 1;
}
