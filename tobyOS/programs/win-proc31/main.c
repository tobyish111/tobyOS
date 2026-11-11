/* win-proc31 -- Track C, W6: CreateProcess. It starts child31.exe (beside
 * it) and, on tobyOS, a native program (Z:\bin\grep) and a Linux one
 * (Z:\bin\linux-hello), and checks what Microsoft documents for
 * CreateProcessA/W, CreatePipe, WaitForSingleObject, GetExitCodeProcess,
 * TerminateProcess, GetProcessId and CloseHandle, and its "Parsing C
 * command-line arguments" -- measured on Windows 11 first (it exits 31
 * there, the tobyOS-only checks skipped):
 *   - lpApplicationName NULL: the first token of the command line is the
 *     program, ".exe" added, found beside the .exe; lpApplicationName given:
 *     that file exactly, the command line's first token only argv[0];
 *   - exit codes back, all 32 bits; a program that is not there: FALSE,
 *     ERROR_FILE_NOT_FOUND;
 *   - a running child: STILL_ACTIVE, WaitForSingleObject's timeout,
 *     TerminateProcess's exit code;
 *   - STARTF_USESTDHANDLES with CreatePipe's handles: the child reads the
 *     parent's input to its end and writes back through another pipe; the
 *     parent's last ReadFile fails with ERROR_BROKEN_PIPE;
 *   - the child's argv by the documented rules, and GetCommandLineA/W the
 *     exact string;
 *   - lpCurrentDirectory; CreateProcessW;
 *   - the name rules as Windows 11 keeps them: a path with no extension gets
 *     .exe (the page says it does not), a trailing period stops it, and a
 *     file that is not a program is ERROR_EXE_MACHINE_TYPE_MISMATCH, and an
 *     unquoted path with a space is read the shorter way first;
 *   - on tobyOS: Z:\bin\grep through pipes, by its path and by its bare
 *     name (no .exe -- tobyOS tries the name as it is after Windows' search
 *     fails), its exit status both ways; a Linux ELF's exit code.
 * Each check prints ok/FAIL with its number; exit 31 iff all pass, else
 * 100 + the first that failed. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int first_fail;
static void check(int n, int cond, const char *what) {
    printf("%s %2d %s\n", cond ? "ok  " : "FAIL", n, what);
    if (!cond && !first_fail) first_fail = n;
}

static char exedir[MAX_PATH];

/* Run `app` (may be NULL) with `cmd`, its standard input fed `input` (NULL:
 * none -- the pipe is closed at once) and its standard output and error
 * captured into out (NUL-terminated). Returns CreateProcess's result; *code
 * gets the exit code, *last the last ReadFile's error. */
static BOOL run(const char *app, const char *cmd, const char *cwd, const char *input,
                char *out, int cap, DWORD *code, DWORD *last) {
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE in_r, in_w, out_r, out_w;
    out[0] = 0;
    *code = 0xDEAD;
    *last = 0;
    if (!CreatePipe(&in_r, &in_w, &sa, 0)) return FALSE;
    if (!CreatePipe(&out_r, &out_w, &sa, 0)) return FALSE;
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = out_w;
    PROCESS_INFORMATION pi;
    char line[600];
    strcpy(line, cmd);                     /* CreateProcessA may write to it */
    BOOL ok = CreateProcessA(app, line, NULL, NULL, TRUE, 0, NULL, cwd, &si, &pi);
    DWORD err = GetLastError();
    CloseHandle(in_r);
    CloseHandle(out_w);
    if (ok && input) {
        DWORD w;
        WriteFile(in_w, input, (DWORD)strlen(input), &w, NULL);
    }
    CloseHandle(in_w);
    int n = 0;
    if (ok) {
        DWORD got;
        for (;;) {
            if (!ReadFile(out_r, out + n, (DWORD)(cap - 1 - n), &got, NULL)) {
                *last = GetLastError();
                break;
            }
            if (got == 0) { *last = 0; break; }
            n += (int)got;
            if (n >= cap - 1) break;
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    out[n] = 0;
    CloseHandle(out_r);
    SetLastError(err);
    return ok;
}

static void show(const char *s) {
    /* (one printf: putc has no tobyOS shim) */
    char b[2600];
    int k = 0;
    for (; *s && k < (int)sizeof b - 3; s++) {
        if (*s == '\n') { b[k++] = '\\'; b[k++] = 'n'; }
        else b[k++] = *s;
    }
    b[k] = 0;
    printf("     [%s]\n", b);
}

int main(void) {
    char self[MAX_PATH];
    GetModuleFileNameA(NULL, self, sizeof self);
    strcpy(exedir, self);
    char *bs = strrchr(exedir, '\\');
    if (bs) bs[1] = 0;
    printf("win-proc31: %s\n", self);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD code = 0;

    /* 1: the search, an exit code, the process information */
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    char c1[] = "child31 exit 42";
    BOOL ok = CreateProcessA(NULL, c1, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    DWORD w = ok ? WaitForSingleObject(pi.hProcess, INFINITE) : 0xFFFFFFFF;
    BOOL gc = ok && GetExitCodeProcess(pi.hProcess, &code);
    check(1, ok && pi.hProcess && pi.hThread && pi.dwProcessId &&
             GetProcessId(pi.hProcess) == pi.dwProcessId && w == WAIT_OBJECT_0 && gc && code == 42 &&
             CloseHandle(pi.hThread) && CloseHandle(pi.hProcess),
          "\"child31 exit 42\": found beside the .exe (.exe added), exit code 42, "
          "GetProcessId = dwProcessId");

    /* 2: lpApplicationName -- the command line's first token is only argv[0] */
    char app[MAX_PATH], out[1200];
    DWORD last;
    strcpy(app, exedir);
    strcat(app, "child31.exe");
    ok = run(app, "anything args x", NULL, NULL, out, sizeof out, &code, &last);
    check(2, ok && code == 0 && !strncmp(out, "argc=3|anything|args|x\n", strlen("argc=3|anything|args|x\n")),
          "lpApplicationName: that file runs, the command line's first token is its argv[0]");
    show(out);

    /* 3: all 32 bits of an exit code */
    ok = run(NULL, "child31 exit 0xC0000005", NULL, NULL, out, sizeof out, &code, &last);
    check(3, ok && code == 0xC0000005, "an exit code of 0xC0000005 arrives whole");

    /* 4, 5: no such program */
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    char c4[] = "nosuch31 x";
    SetLastError(0);
    ok = CreateProcessA(NULL, c4, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    DWORD e4 = GetLastError();
    check(4, !ok && e4 == ERROR_FILE_NOT_FOUND, "a program that is nowhere: FALSE, ERROR_FILE_NOT_FOUND");
    strcpy(app, exedir);
    strcat(app, "child31");
    char c5[] = "child31 exit 1";
    SetLastError(0);
    ok = CreateProcessA(app, c5, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    DWORD e5 = GetLastError();
    if (ok) { WaitForSingleObject(pi.hProcess, INFINITE); CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    check(5, !ok && e5 == ERROR_FILE_NOT_FOUND,
          "lpApplicationName with no extension: no .exe is assumed, ERROR_FILE_NOT_FOUND");
    printf("     (errors %lu, %lu)\n", (unsigned long)e4, (unsigned long)e5);

    /* 6: a child that is still running */
    char c6[] = "child31 sleep 3000";
    ok = CreateProcessA(NULL, c6, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    DWORD a6 = 0, w6 = 0, w6b = 0, c6code = 0;
    BOOL t6 = FALSE;
    if (ok) {
        GetExitCodeProcess(pi.hProcess, &a6);
        w6 = WaitForSingleObject(pi.hProcess, 100);
        t6 = TerminateProcess(pi.hProcess, 77);
        w6b = WaitForSingleObject(pi.hProcess, 1000);   /* well before its 3 s sleep ends */
        GetExitCodeProcess(pi.hProcess, &c6code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    check(6, ok && a6 == STILL_ACTIVE && w6 == WAIT_TIMEOUT && t6 && w6b == WAIT_OBJECT_0 && c6code == 77,
          "a running child: STILL_ACTIVE, a 100 ms wait times out, TerminateProcess(77) ends it with 77");

    /* 7: pipes both ways */
    ok = run(NULL, "child31 upper", NULL, "hello pipe\nsecond line\n", out, sizeof out, &code, &last);
    check(7, ok && code == 0 && !strcmp(out, "HELLO PIPE\nSECOND LINE\ndone\n") && last == ERROR_BROKEN_PIPE,
          "STARTF_USESTDHANDLES + CreatePipe: the child reads to the end, writes back; "
          "the last ReadFile fails, ERROR_BROKEN_PIPE");
    show(out);
    printf("     (last ReadFile error %lu)\n", (unsigned long)last);

    /* 8: the command-line rules, and GetCommandLine */
    const char *c8 = "child31 args \"a b\"  c\\\"d e\\\\\"f g\" h\\i \"\" \"j\"\"k\" end";
    ok = run(NULL, c8, NULL, NULL, out, sizeof out, &code, &last);
    const char *want8 = "argc=9|child31|args|a b|c\"d|e\\f g|h\\i||j\"k|end\n";
    char cmdwant[700];
    strcpy(cmdwant, want8);
    strcat(cmdwant, "cmd=");
    strcat(cmdwant, c8);
    strcat(cmdwant, "\ncmdw=");
    strcat(cmdwant, c8);
    strcat(cmdwant, "\n");
    check(8, ok && code == 0 && !strcmp(out, cmdwant),
          "the documented argument rules (quotes, \\\", 2n backslashes, \"\" inside quotes); "
          "GetCommandLineA/W: the exact string");
    show(out);

    /* 9: lpCurrentDirectory */
    ok = run(NULL, "child31 cwd", "C:\\", NULL, out, sizeof out, &code, &last);
    check(9, ok && code == 0 && !strcmp(out, "C:\\\n"), "lpCurrentDirectory C:\\ is the child's");
    show(out);

    /* 10: CreateProcessW */
    STARTUPINFOW siw;
    memset(&siw, 0, sizeof siw);
    siw.cb = sizeof siw;
    WCHAR c10[] = L"child31 exit 5";
    ok = CreateProcessW(NULL, c10, NULL, NULL, FALSE, 0, NULL, NULL, &siw, &pi);
    code = 0;
    if (ok) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    check(10, ok && code == 5, "CreateProcessW: the same, exit code 5");

    /* 11: handles after CloseHandle; this process's own */
    ok = CreateProcessA(NULL, c1, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    DWORD self_code = 0;
    BOOL again = TRUE;
    if (ok) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        DWORD dummy;
        again = GetExitCodeProcess(pi.hProcess, &dummy);
    }
    check(11, ok && !again && GetExitCodeProcess(GetCurrentProcess(), &self_code) && self_code == STILL_ACTIVE,
          "a closed process handle is no handle; GetCurrentProcess() is STILL_ACTIVE");

    /* 12-14: the name rules Windows 11 keeps (Microsoft's page says a name
     * with a path gets no .exe; Windows adds it all the same) */
    char c12[MAX_PATH + 32];
    strcpy(c12, exedir);
    strcat(c12, "child31 exit 3");
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    ok = CreateProcessA(NULL, c12, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    code = 0;
    if (ok) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    check(12, ok && code == 3, "a full path with no extension: .exe is added (exit code 3)");
    char c13[] = "child31. exit 8";
    SetLastError(0);
    ok = CreateProcessA(NULL, c13, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    DWORD e13 = GetLastError();
    if (ok) { WaitForSingleObject(pi.hProcess, INFINITE); CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    check(13, !ok && e13 == ERROR_FILE_NOT_FOUND, "a name ending in a period: no .exe, ERROR_FILE_NOT_FOUND");
    char c14[] = "notpe31 x";
    SetLastError(0);
    ok = CreateProcessA(NULL, c14, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    DWORD e14 = GetLastError();
    if (ok) { WaitForSingleObject(pi.hProcess, INFINITE); CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    check(14, !ok && e14 == ERROR_EXE_MACHINE_TYPE_MISMATCH,
          "notpe31.exe, a text file: FALSE, ERROR_EXE_MACHINE_TYPE_MISMATCH (216)");
    printf("     (errors %lu, %lu)\n", (unsigned long)e13, (unsigned long)e14);

    /* 15: an unquoted path with a space -- "<temp>w6 sp\child31 args x" is
     * tried as "<temp>w6.exe" first, then "<temp>w6 sp\child31.exe"; and the
     * child's command line has that part quoted, so its argv[0] is the whole
     * path (Windows 11 does so; the caller's buffer stays as it was) */
    char tmp[MAX_PATH], dir[MAX_PATH], dst[MAX_PATH], src[MAX_PATH], c15[MAX_PATH + 32];
    GetTempPathA(sizeof tmp, tmp);
    strcpy(dir, tmp);
    if (dir[0] && dir[strlen(dir) - 1] != '\\') strcat(dir, "\\");
    strcat(dir, "w6 sp");
    CreateDirectoryA(dir, NULL);                   /* (it may be there already) */
    strcpy(dst, dir);
    strcat(dst, "\\child31.exe");
    strcpy(src, exedir);
    strcat(src, "child31.exe");
    BOOL copied = FALSE;
    {
        HANDLE in = CreateFileA(src, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        HANDLE outf = CreateFileA(dst, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (in != INVALID_HANDLE_VALUE && outf != INVALID_HANDLE_VALUE) {
            static char cb[16384];
            DWORD got, put;
            copied = TRUE;
            while (ReadFile(in, cb, sizeof cb, &got, NULL) && got > 0)
                if (!WriteFile(outf, cb, got, &put, NULL) || put != got) { copied = FALSE; break; }
        }
        if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
        if (outf != INVALID_HANDLE_VALUE) CloseHandle(outf);
    }
    strcpy(c15, dir);
    strcat(c15, "\\child31 args x");
    char want15[3 * MAX_PATH + 64];
    strcpy(want15, "argc=3|");
    strcat(want15, dir);
    strcat(want15, "\\child31|args|x\ncmd=\"");
    strcat(want15, dir);
    strcat(want15, "\\child31\" args x\ncmdw=\"");
    strcat(want15, dir);
    strcat(want15, "\\child31\" args x\n");
    ok = copied && run(NULL, c15, NULL, NULL, out, sizeof out, &code, &last);
    DeleteFileA(dst);
    check(15, ok && code == 0 && !strcmp(out, want15),
          "an unquoted path with a space in a directory name: the shorter reading first, then "
          "the longer; the child's command line quotes it, its argv[0] the whole path");
    show(out);
    printf("     (copied %d)\n", copied);

    /* 16-18: tobyOS -- a native program and a Linux one */
    if (GetFileAttributesA("Z:\\bin\\grep") != INVALID_FILE_ATTRIBUTES) {
        ok = run(NULL, "Z:\\bin\\grep -c o", NULL, "foo\nbar\nboo\n", out, sizeof out, &code, &last);
        check(16, ok && code == 0 && !strcmp(out, "2\n"),
              "Z:\\bin\\grep -c o through pipes: \"2\", exit status 0");
        show(out);
        ok = run(NULL, "grep -c zzz", NULL, "foo\nbar\n", out, sizeof out, &code, &last);
        check(17, ok && code == 1 && !strcmp(out, "0\n"),
              "\"grep\" by its bare name (no grep.exe): found beside the .exe; no match, exit status 1");
        show(out);
        ok = run(NULL, "Z:\\bin\\linux-hello", NULL, NULL, out, sizeof out, &code, &last);
        check(18, ok && code == 42, "a Linux ELF (Z:\\bin\\linux-hello): its exit code, 42");
        printf("     (exit %lu)\n", (unsigned long)code);
    } else {
        printf("skip 16-18 (not tobyOS: no Z:\\bin\\grep)\n");
    }

    if (first_fail) {
        printf("win-proc31: check %d failed\n", first_fail);
        return 100 + first_fail;
    }
    printf("win-proc31: all checks ok\n");
    return 31;
}
