// win-cxx44 -- Track C, W7m: the rest of what Microsoft's msvcp140.dll asks
// of the system. Built by cl.exe (/EHsc /MD /std:c++17) against Microsoft's
// redistributable msvcp140.dll, vcruntime140.dll and vcruntime140_1.dll:
// file streams (text and binary, seeking, their buffer), std::filesystem, std::async (the
// Windows thread pool under it) and the thread pool itself, wide streams and
// wide paths, the time facets (put_time, get_time), the "C" locale's facets,
// and the kernel32 and C runtime calls under them. Each check is what Windows
// 11 does (measured first: it exits 46 there). Exit 46 iff all pass, else 100
// + the first that failed.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>
#include <direct.h>
#include <io.h>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <filesystem>
#include <future>
#include <thread>
#include <atomic>
#include <chrono>
#include <locale>
#include <ctime>

namespace fs = std::filesystem;

static int first_fail;
static void check(int n, bool ok, const char *what) {
    printf("%s %2d %s\n", ok ? "ok  " : "FAIL", n, what);
    if (!ok && !first_fail) first_fail = n;
}
static std::string slurp(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}
static std::string vis(const std::string &s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '\r') o += "\\r";
        else if (c == '\n') o += "\\n";
        else if (c < 32 || c > 126) { char b[8]; snprintf(b, sizeof b, "\\x%02X", c); o += b; }
        else o += (char)c;
    }
    return o;
}

static char g_self[MAX_PATH];
// Run this program as a child with `mode`, its stdout a pipe; what it wrote
static int run_child(const char *mode, std::string &out) {
    SECURITY_ATTRIBUTES sa = { sizeof sa, 0, TRUE };
    HANDLE r, w;
    if (!CreatePipe(&r, &w, &sa, 0)) return -1;
    SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE); si.hStdOutput = w; si.hStdError = w;
    PROCESS_INFORMATION pi;
    char cmd[MAX_PATH + 32];
    snprintf(cmd, sizeof cmd, "\"%s\" %s", g_self, mode);
    if (!CreateProcessA(g_self, cmd, 0, 0, TRUE, 0, 0, 0, &si, &pi)) { CloseHandle(r); CloseHandle(w); return -2; }
    CloseHandle(w);
    char b[512]; DWORD got;
    out.clear();
    while (ReadFile(r, b, sizeof b, &got, 0) && got) out.append(b, got);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(r);
    return (int)code;
}

// ---- the thread pool's callbacks ----
static std::atomic<int> g_work, g_timer, g_wait;
static std::atomic<DWORD> g_work_tid;
static TP_WAIT_RESULT g_wait_res[4];
static void CALLBACK work_cb(PTP_CALLBACK_INSTANCE, PVOID ctx, PTP_WORK) {
    Sleep(5);
    g_work += (int)(intptr_t)ctx;
    g_work_tid = GetCurrentThreadId();
}
static void CALLBACK timer_cb(PTP_CALLBACK_INSTANCE, PVOID ctx, PTP_TIMER) {
    g_timer += (int)(intptr_t)ctx;
}
static void CALLBACK wait_cb(PTP_CALLBACK_INSTANCE, PVOID, PTP_WAIT, TP_WAIT_RESULT res) {
    int k = g_wait++;
    if (k < 4) g_wait_res[k] = res;
}

static int child(const char *m) {
    if (!strcmp(m, "wcout")) {                       // 5: std::wcout, text mode
        std::wcout << L"wide " << 42 << L"\n";
        std::wcout.flush();
        return 5;
    }
    if (!strcmp(m, "cout")) {                        // 1: std::cout, text mode
        std::cout << "line1\nline2" << std::endl;
        return 6;
    }
    if (!strncmp(m, "fl-", 3)) {                     // 11: what a stream holds at the end
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        fs::path p = fs::path(tmp) / L"w7m-fl.txt";
        FILE *f = _wfopen(p.c_str(), L"w");
        fputs("pending", f);
        printf("printf ");
        DWORD w;
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "writefile ", 10, &w, 0);
        m += 3;
        if (!strcmp(m, "return")) return 11;
        if (!strcmp(m, "exit")) exit(12);
        if (!strcmp(m, "_exit")) _exit(13);
        if (!strcmp(m, "ExitProcess")) ExitProcess(14);
        if (!strcmp(m, "TerminateProcess")) TerminateProcess(GetCurrentProcess(), 15);
        return 16;
    }
    return 1;
}

int main(int argc, char **argv) {
    GetModuleFileNameA(0, g_self, sizeof g_self);
    if (argc > 1) return child(argv[1]);
    wchar_t tmpw[MAX_PATH];
    GetTempPathW(MAX_PATH, tmpw);
    fs::path base = fs::path(tmpw) / L"w7m";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);

    // 1: file streams: text and binary, std::cout in a child
    {
        fs::path t = base / "t.txt", b = base / "b.bin";
        { std::ofstream o(t); o << "a\nb\n" << 12 << '\n'; }
        { std::ofstream o(b, std::ios::binary); o << "a\nb\n"; }
        std::string st = slurp(t), sb = slurp(b);
        std::ifstream in(t);
        std::string l1, l2, l3, l4;
        std::getline(in, l1); std::getline(in, l2); std::getline(in, l3);
        bool more = (bool)std::getline(in, l4);
        std::string co;
        int rc = run_child("cout", co);
        printf("     text \"%s\", binary \"%s\", lines [%s][%s][%s] more %d; child cout \"%s\" (%d)\n",
               vis(st).c_str(), vis(sb).c_str(), l1.c_str(), l2.c_str(), l3.c_str(), (int)more, vis(co).c_str(), rc);
        check(1, st == "a\r\nb\r\n12\r\n" && sb == "a\nb\n" && l1 == "a" && l2 == "b" && l3 == "12" && !more &&
                 rc == 6 && co == "line1\r\nline2\r\n",
              "ofstream writes text ('\\n' as \"\\r\\n\") unless binary; ifstream's getline reads it back; std::cout too");
    }

    // 2: seeking: fstream's seekg/tellg/seekp over a binary file
    {
        fs::path p = base / "s.bin";
        { std::ofstream o(p, std::ios::binary); for (int i = 0; i < 1000; i++) o.put((char)('A' + i % 26)); }
        std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
        f.seekg(500);
        char c1 = (char)f.get();
        std::streamoff t1 = f.tellg();
        f.seekg(0, std::ios::end);
        std::streamoff end = f.tellg();
        f.seekp(10);
        f.put('#');
        f.seekg(-3, std::ios::end);
        char c2 = (char)f.get();
        f.seekg(10);
        char c3 = (char)f.get();
        f.close();
        std::string all = slurp(p);
        printf("     get %c at 500, tellg %lld, end %lld, then %c %c, size %zu\n", c1, (long long)t1, (long long)end, c2, c3, all.size());
        check(2, c1 == 'A' + 500 % 26 && t1 == 501 && end == 1000 && c2 == 'A' + 997 % 26 && c3 == '#' &&
                 all.size() == 1000 && all[10] == '#',
              "fstream: seekg/tellg/seekp (from the start, the end), get/put after them");
    }

    // 3: std::filesystem
    {
        fs::path d = base / "fsdir", sub = d / "sub" / "deep";
        bool made = fs::create_directories(sub, ec);
        { std::ofstream(d / "one.txt", std::ios::binary) << "12345"; }
        { std::ofstream(d / "sub" / "two.txt", std::ios::binary) << "1234567890"; }
        bool ex = fs::exists(d / "one.txt") && fs::is_directory(sub) && !fs::exists(d / "nope");
        uintmax_t sz = fs::file_size(d / "one.txt");
        std::vector<std::string> names;
        for (auto &e : fs::directory_iterator(d)) names.push_back(e.path().filename().string() + (e.is_directory() ? "/" : ""));
        std::sort(names.begin(), names.end());
        std::string ls;
        for (auto &n : names) ls += n + " ";
        int rec = 0;
        for (auto &e : fs::recursive_directory_iterator(d)) { (void)e; rec++; }
        fs::copy_file(d / "one.txt", d / "copy.txt", ec);
        bool cp = !ec && fs::file_size(d / "copy.txt") == 5;
        fs::rename(d / "copy.txt", d / "moved.txt", ec);
        bool mv = !ec && fs::exists(d / "moved.txt") && !fs::exists(d / "copy.txt");
        fs::resize_file(d / "moved.txt", 2, ec);
        bool rs = !ec && fs::file_size(d / "moved.txt") == 2;
        fs::create_hard_link(d / "one.txt", d / "hard.txt", ec);
        bool hl = !ec && fs::hard_link_count(d / "one.txt") == 2 && fs::equivalent(d / "one.txt", d / "hard.txt");
        fs::path cwd0 = fs::current_path();
        fs::current_path(d);
        bool cd = fs::current_path() == d && fs::exists("one.txt");
        fs::current_path(cwd0);
        fs::space_info si = fs::space(d);
        bool sp = si.capacity > 0 && si.available <= si.capacity;
        auto t1 = fs::last_write_time(d / "one.txt");
        Sleep(30);
        { std::ofstream(d / "later.txt") << "x"; }
        auto t2 = fs::last_write_time(d / "later.txt");
        bool tm = t2 > t1;
        bool rm1 = fs::remove(d / "later.txt");
        uintmax_t gone = fs::remove_all(d);
        bool rm = rm1 && gone == 7 && !fs::exists(d);
        printf("     made %d exists %d size %llu [%s] recursive %d; copy %d rename %d resize %d hardlink %d cd %d space %d time %d remove %d (%llu)\n",
               (int)made, (int)ex, (unsigned long long)sz, ls.c_str(), rec, cp, mv, rs, hl, cd, sp, tm, rm, (unsigned long long)gone);
        check(3, made && ex && sz == 5 && ls == "one.txt sub/ " && rec == 4 && cp && mv && rs && hl && cd && sp && tm && rm,
              "std::filesystem: create_directories, exists, file_size, the directory iterators, copy, rename, resize, "
              "a hard link, current_path, space, last_write_time, remove_all");
    }

    // 4: std::async, and the thread pool under it
    {
        DWORD me = GetCurrentThreadId();
        std::vector<std::future<long long>> fu;
        for (int k = 0; k < 6; k++)
            fu.push_back(std::async(std::launch::async, [k] {
                long long s = 0;
                for (int i = 1; i <= 10000; i++) s += i * (k + 1);
                return s;
            }));
        long long total = 0;
        for (auto &f : fu) total += f.get();
        auto other = std::async(std::launch::async, [] { return GetCurrentThreadId(); });
        DWORD otid = other.get();
        bool thrown = false;
        auto bad = std::async(std::launch::async, []() -> int { throw std::runtime_error("from a task"); });
        try { bad.get(); } catch (const std::runtime_error &e) { thrown = !strcmp(e.what(), "from a task"); }
        auto deferred = std::async(std::launch::deferred, [] { return GetCurrentThreadId(); });
        DWORD dtid = deferred.get();
        printf("     sum %lld (want %lld), another thread %d, the exception %d, deferred here %d\n",
               total, 50005000LL * 21, otid != me, (int)thrown, dtid == me);
        check(4, total == 50005000LL * 21 && otid != me && thrown && dtid == me,
              "std::async: six tasks on other threads, an exception through the future, launch::deferred on the caller's");
    }

    // 5: the thread pool: work, a timer, a wait
    {
        PTP_WORK w = CreateThreadpoolWork(work_cb, (PVOID)(intptr_t)3, 0);
        for (int i = 0; i < 4; i++) SubmitThreadpoolWork(w);
        WaitForThreadpoolWorkCallbacks(w, FALSE);
        int works = g_work;
        bool other = g_work_tid != GetCurrentThreadId();
        CloseThreadpoolWork(w);
        PTP_TIMER t = CreateThreadpoolTimer(timer_cb, (PVOID)(intptr_t)1, 0);
        // 250 ms from now: "set, not yet run" holds only while this thread gets
        // back from SetThreadpoolTimer before the due time. 30 ms did not on the
        // EliteDesk: the boot harnesses run before its serial console turns
        // asynchronous, so the kernel's line for the pool's new thread holds
        // the caller ~32 ms at 38400 baud, and the 30 ms timer won or lost
        // that race by chance (one box run each way). The checks are the same.
        ULARGE_INTEGER due; due.QuadPart = (ULONGLONG)-(250 * 10000LL);
        FILETIME ft; ft.dwLowDateTime = due.LowPart; ft.dwHighDateTime = due.HighPart;
        SetThreadpoolTimer(t, &ft, 0, 0);
        bool set = IsThreadpoolTimerSet(t);
        int before = g_timer;
        Sleep(600);
        int after = g_timer;
        SetThreadpoolTimer(t, 0, 0, 0);
        WaitForThreadpoolTimerCallbacks(t, TRUE);
        CloseThreadpoolTimer(t);
        HANDLE ev = CreateEventW(0, FALSE, FALSE, 0);
        PTP_WAIT wt = CreateThreadpoolWait(wait_cb, 0, 0);
        SetThreadpoolWait(wt, ev, 0);
        Sleep(30);
        SetEvent(ev);
        Sleep(100);
        ULARGE_INTEGER to; to.QuadPart = (ULONGLONG)-(20 * 10000LL);
        FILETIME tft; tft.dwLowDateTime = to.LowPart; tft.dwHighDateTime = to.HighPart;
        SetThreadpoolWait(wt, ev, &tft);
        Sleep(150);
        WaitForThreadpoolWaitCallbacks(wt, FALSE);
        CloseThreadpoolWait(wt);
        CloseHandle(ev);
        printf("     work %d (another thread %d); timer set %d, %d before, %d after; waits %d: %lu %lu\n",
               works, (int)other, (int)set, before, after, (int)g_wait, (unsigned long)g_wait_res[0], (unsigned long)g_wait_res[1]);
        check(5, works == 12 && other && set && before == 0 && after == 1 &&
                 g_wait == 2 && g_wait_res[0] == WAIT_OBJECT_0 && g_wait_res[1] == WAIT_TIMEOUT,
              "the thread pool: work submitted 4 times on its threads, a one-shot timer, a wait signalled then timed out");
    }

    // 6: wide streams and wide paths
    {
        static const wchar_t wname[] = { L'w', 0xE9, L'.', L't', L'x', L't', 0 };   // "w" e-acute ".txt": past ASCII
        fs::path p = base / wname;
        { std::wofstream o(p); o << L"wide line\n" << 7 << L'\n'; }
        std::string raw = slurp(p);
        bool listed = false;
        for (auto &e : fs::directory_iterator(base)) if (e.path().filename().wstring() == wname) listed = true;
        std::wifstream in(p);
        std::wstring a, b;
        std::getline(in, a); std::getline(in, b);
        std::string co;
        int rc = run_child("wcout", co);
        FILE *f = _wfopen(p.c_str(), L"r");
        wint_t c1 = fgetwc(f);
        wint_t u = ungetwc(L'Z', f);
        wint_t c2 = fgetwc(f), c3 = fgetwc(f);
        fclose(f);
        printf("     raw \"%s\", listed %d, read [%ls][%ls], wcout \"%s\" (%d); fgetwc %c ungetwc %c then %c %c\n",
               vis(raw).c_str(), (int)listed, a.c_str(), b.c_str(), vis(co).c_str(), rc, (int)c1, (int)u, (int)c2, (int)c3);
        check(6, raw == "wide line\r\n7\r\n" && listed && a == L"wide line" && b == L"7" && rc == 5 && co == "wide 42\r\n" &&
                 c1 == L'w' && u == L'Z' && c2 == L'Z' && c3 == L'i',
              "wofstream/wifstream on a wide path past ASCII (the directory lists it back), std::wcout; fgetwc and ungetwc");
    }

    // 7: the time facets: put_time, get_time, strftime, wcsftime
    {
        std::tm tm = {};
        tm.tm_year = 124; tm.tm_mon = 2; tm.tm_mday = 5; tm.tm_hour = 14; tm.tm_min = 7; tm.tm_sec = 9; tm.tm_wday = 2; tm.tm_yday = 64;
        std::ostringstream os;
        os << std::put_time(&tm, "%Y-%m-%d %H:%M:%S %A %a %B %b %p %I %j %y|%c|%x|%X");
        std::wostringstream wos;
        wos << std::put_time(&tm, L"%A %B %d");
        char sb[100]; size_t sn = strftime(sb, sizeof sb, "%a %b %e %H:%M:%S %Y %%", &tm);
        wchar_t wb[64]; size_t wn = wcsftime(wb, 64, L"%B %Y", &tm);
        std::tm g = {};
        std::istringstream is("2023-11-30 08:15:59 Wednesday August");
        is >> std::get_time(&g, "%Y-%m-%d %H:%M:%S %A %B");
        bool gok = !is.fail() && g.tm_year == 123 && g.tm_mon == 7 && g.tm_mday == 30 && g.tm_hour == 8 &&
                   g.tm_min == 15 && g.tm_sec == 59 && g.tm_wday == 3;
        std::string s = os.str();
        printf("     put_time \"%s\"\n     wide \"%ls\", strftime \"%s\" (%zu), wcsftime \"%ls\" (%zu), get_time %d\n",
               s.c_str(), wos.str().c_str(), sb, sn, wb, wn, (int)gok);
        check(7, s == "2024-03-05 14:07:09 Tuesday Tue March Mar PM 02 065 24|03/05/24 14:07:09|03/05/24|14:07:09" &&
                 wos.str() == L"Tuesday March 05" && !strcmp(sb, "Tue Mar  5 14:07:09 2024 %") && sn == 26 &&
                 !wcscmp(wb, L"March 2024") && wn == 10 && gok,
              "put_time and get_time in the \"C\" locale (day and month names, %c %x %X), strftime, wcsftime");
    }

    // 8: the "C" locale's facets, the kernel32 calls under them
    {
        std::locale loc = std::locale::classic();
        auto &wct = std::use_facet<std::ctype<wchar_t>>(loc);
        bool ct = wct.toupper(L'q') == L'Q' && wct.tolower(L'Q') == L'q' && wct.is(std::ctype_base::alpha, L'z') &&
                  wct.is(std::ctype_base::digit, L'7') && !wct.is(std::ctype_base::alpha, L'7') &&
                  wct.is(std::ctype_base::space, L'\t') && wct.widen('x') == L'x' && wct.narrow(L'y', '?') == 'y';
        auto &col = std::use_facet<std::collate<char>>(loc);
        std::string x = "apple", y = "banana";
        bool co = col.compare(x.data(), x.data() + x.size(), y.data(), y.data() + y.size()) < 0 &&
                  col.compare(y.data(), y.data() + y.size(), x.data(), x.data() + x.size()) > 0;
        auto &wcol = std::use_facet<std::collate<wchar_t>>(loc);
        std::wstring wx = L"abc", wy = L"abd";
        bool wco = wcol.compare(wx.data(), wx.data() + 3, wy.data(), wy.data() + 3) < 0;
        WORD types[4] = {};
        BOOL gs = GetStringTypeW(CT_CTYPE1, L"a1 Z", 4, types);
        bool tok = gs && (types[0] & C1_LOWER) && (types[0] & C1_ALPHA) && (types[1] & C1_DIGIT) &&
                   (types[2] & C1_SPACE) && (types[3] & C1_UPPER);
        wchar_t up[8] = {};
        int lm = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, L"mixEd", 5, up, 8, 0, 0, 0);
        int cmp1 = CompareStringEx(LOCALE_NAME_INVARIANT, NORM_IGNORECASE, L"Hello", -1, L"hELLO", -1, 0, 0, 0);
        int cmp2 = CompareStringEx(LOCALE_NAME_INVARIANT, 0, L"abc", -1, L"abd", -1, 0, 0, 0);
        wchar_t dec[8] = {};
        int gl = GetLocaleInfoEx(LOCALE_NAME_INVARIANT, LOCALE_SDECIMAL, dec, 8);
        CPINFO cpi = {};
        BOOL gc = GetCPInfo(1252, &cpi);
        BOOL gu = FALSE; CPINFO cpu = {};
        gu = GetCPInfo(CP_UTF8, &cpu);
        printf("     ctype %d collate %d/%d types %d map \"%ls\" (%d) compare %d %d decimal \"%ls\" (%d) cp1252 %d/%u utf8 %d/%u\n",
               ct, co, wco, tok, up, lm, cmp1, cmp2, dec, gl, gc, cpi.MaxCharSize, gu, cpu.MaxCharSize);
        check(8, ct && co && wco && tok && lm == 5 && !wcscmp(up, L"MIXED") && cmp1 == CSTR_EQUAL &&
                 cmp2 == CSTR_LESS_THAN && gl == 2 && !wcscmp(dec, L".") && gc && cpi.MaxCharSize == 1 &&
                 gu && cpu.MaxCharSize == 4,
              "the \"C\" locale's ctype<wchar_t> and collate; GetStringTypeW, LCMapStringEx, CompareStringEx, GetLocaleInfoEx, GetCPInfo");
    }

    // 9: the C runtime's and kernel32's calls under them, by hand
    {
        float f = strtof("2.5e1", 0);
        float lg = logf(1.0f);
        wchar_t *dup = _wcsdup(L"dup");
        bool d = dup && !wcscmp(dup, L"dup");
        free(dup);
        wchar_t cw0[MAX_PATH]; _wgetcwd(cw0, MAX_PATH);
        fs::path wd = base / L"wd";
        _wmkdir(wd.c_str());
        int ch = _wchdir(wd.c_str());
        wchar_t cw1[MAX_PATH]; _wgetcwd(cw1, MAX_PATH);
        _wchdir(cw0);
        bool cdok = ch == 0 && fs::path(cw1) == wd;
        { std::ofstream((wd / "r1").string()) << "r"; }
        int rn = _wrename((wd / L"r1").c_str(), (wd / L"r2").c_str());
        int rm = _wremove((wd / L"r2").c_str());
        int rd = _wrmdir(wd.c_str());
        bool fsok = rn == 0 && rm == 0 && rd == 0 && !fs::exists(wd);
        void *frames[8] = {};
        USHORT nf = RtlCaptureStackBackTrace(0, 8, frames, 0);
        HMODULE hm = 0;
        BOOL gm = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     (LPCWSTR)(void *)&main, &hm);
        bool st = nf >= 2 && frames[0] && gm && hm == GetModuleHandleW(0);
        HANDLE e = CreateEventExW(0, 0, CREATE_EVENT_MANUAL_RESET | CREATE_EVENT_INITIAL_SET, EVENT_ALL_ACCESS);
        DWORD w1 = WaitForSingleObject(e, 0), w2 = WaitForSingleObject(e, 0);
        HANDLE s = CreateSemaphoreExW(0, 1, 2, 0, 0, SEMAPHORE_ALL_ACCESS);
        DWORD w3 = WaitForSingleObject(s, 0), w4 = WaitForSingleObject(s, 0);
        CloseHandle(e); CloseHandle(s);
        bool sync = w1 == WAIT_OBJECT_0 && w2 == WAIT_OBJECT_0 && w3 == WAIT_OBJECT_0 && w4 == WAIT_TIMEOUT;
        printf("     strtof %g logf %g wcsdup %d dirs %d files %d stack %d frames, module %d; events %d\n",
               (double)f, (double)lg, d, cdok, fsok, (int)nf, (int)st, sync);
        check(9, f == 25.0f && lg == 0.0f && d && cdok && fsok && st && sync,
              "strtof, logf, _wcsdup, _wgetcwd/_wchdir/_wrename/_wremove/_wrmdir, RtlCaptureStackBackTrace, GetModuleHandleExW, "
              "CreateEventExW, CreateSemaphoreExW");
    }

    // 10: kernel32's file information calls
    {
        fs::path p = base / "info.bin";
        { std::ofstream(p, std::ios::binary) << "0123456789"; }
        HANDLE h = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_DELETE, 0,
                               OPEN_EXISTING, 0, 0);
        BY_HANDLE_FILE_INFORMATION bi = {};
        BOOL g1 = GetFileInformationByHandle(h, &bi);
        FILE_STANDARD_INFO sti = {};
        BOOL g2 = GetFileInformationByHandleEx(h, FileStandardInfo, &sti, sizeof sti);
        FILE_END_OF_FILE_INFO eof; eof.EndOfFile.QuadPart = 4;
        BOOL s1 = SetFileInformationByHandle(h, FileEndOfFileInfo, &eof, sizeof eof);
        FILE_STANDARD_INFO st2 = {};
        GetFileInformationByHandleEx(h, FileStandardInfo, &st2, sizeof st2);
        FILE_DISPOSITION_INFO di; di.DeleteFile = TRUE;
        BOOL s2 = SetFileInformationByHandle(h, FileDispositionInfo, &di, sizeof di);
        CloseHandle(h);
        bool gone = !fs::exists(p);
        fs::path c = base / "c.bin";
        { std::ofstream(c, std::ios::binary) << "abc"; }
        BOOL cf = CopyFileW(c.c_str(), (base / "c2.bin").c_str(), TRUE);
        BOOL cf2 = CopyFileW(c.c_str(), (base / "c2.bin").c_str(), TRUE);
        DWORD e2 = GetLastError();
        BOOL sa = SetFileAttributesW(c.c_str(), FILE_ATTRIBUTE_READONLY);
        DWORD at = GetFileAttributesW(c.c_str());
        SetFileAttributesW(c.c_str(), FILE_ATTRIBUTE_NORMAL);
        DWORD at2 = GetFileAttributesW(c.c_str());
        ULARGE_INTEGER fa, tot, fr;
        BOOL df = GetDiskFreeSpaceExW(base.c_str(), &fa, &tot, &fr);
        WIN32_FIND_DATAW fd;
        HANDLE fh = FindFirstFileExW((base / L"c*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, 0, 0);
        std::vector<std::wstring> found;
        if (fh != INVALID_HANDLE_VALUE) {
            do found.push_back(fd.cFileName); while (FindNextFileW(fh, &fd));
            FindClose(fh);
        }
        std::sort(found.begin(), found.end());
        BOOL dw = CreateDirectoryW((base / L"nd").c_str(), 0);
        BOOL dw2 = CreateDirectoryW((base / L"nd").c_str(), 0);
        DWORD e3 = GetLastError();
        printf("     info %d/%d size %llu links %lu, truncated to %lld, deleted %d %d; copy %d then %d (%lu); readonly %lx then %lx; space %d; found %zu; mkdir %d %d (%lu)\n",
               g1, g2, (unsigned long long)sti.EndOfFile.QuadPart, bi.nNumberOfLinks, (long long)st2.EndOfFile.QuadPart, s2, (int)gone,
               cf, cf2, e2, at, at2, df, found.size(), dw, dw2, e3);
        check(10, g1 && g2 && sti.EndOfFile.QuadPart == 10 && bi.nNumberOfLinks == 1 && bi.nFileSizeLow == 10 && s1 &&
                  st2.EndOfFile.QuadPart == 4 && s2 && gone && cf && !cf2 && e2 == ERROR_FILE_EXISTS && sa &&
                  (at & FILE_ATTRIBUTE_READONLY) && !(at2 & FILE_ATTRIBUTE_READONLY) && df && tot.QuadPart > 0 &&
                  found.size() == 2 && found[0] == L"c.bin" && found[1] == L"c2.bin" && dw && !dw2 && e3 == ERROR_ALREADY_EXISTS,
              "GetFileInformationByHandle(Ex), SetFileInformationByHandle (end of file, delete), CopyFileW, SetFileAttributesW, "
              "GetDiskFreeSpaceExW, FindFirstFileExW/FindNextFileW, CreateDirectoryW");
    }

    // 11: a stream's buffer: stdout to a pipe waits (WriteFile's text comes
    //     first), and what a file stream holds reaches the file at return,
    //     exit, _exit and ExitProcess -- not at TerminateProcess
    {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        fs::path p = fs::path(tmp) / L"w7m-fl.txt";
        static const char *const how[5] = { "return", "exit", "_exit", "ExitProcess", "TerminateProcess" };
        bool ok = true;
        for (int i = 0; i < 5; i++) {
            fs::remove(p, ec);
            std::string out, mode = std::string("fl-") + how[i];
            int rc = run_child(mode.c_str(), out);
            std::string in = slurp(p);
            bool term = i == 4;
            bool good = rc == 11 + i && in == (term ? "" : "pending") &&
                        out == (term ? "writefile " : "writefile printf ");
            printf("     %-16s exit %d, the file \"%s\", stdout \"%s\"\n", how[i], rc, in.c_str(), out.c_str());
            ok = ok && good;
        }
        fs::remove(p, ec);
        check(11, ok, "a stream's buffer: stdout to a pipe after WriteFile's text; a file's bytes written at return, "
                      "exit, _exit and ExitProcess, lost at TerminateProcess");
    }

    fs::remove_all(base, ec);
    printf(first_fail ? "SOME FAILED (first %d)\n" : "ALL PASS\n", first_fail);
    return first_fail ? 100 + first_fail : 46;
}
