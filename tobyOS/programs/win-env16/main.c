/* main.c -- a STOCK Win32 console .exe exercising the command line + environment
 * (Track C / C16c): GetCommandLineA, Set/GetEnvironmentVariableA, and
 * ExpandEnvironmentStringsA against tobyOS's per-app environment block.
 *
 * Exit code:
 *   2  GetCommandLineA was empty
 *   3  SetEnvironmentVariableA failed
 *   4  GetEnvironmentVariableA didn't read the value back
 *   5  a seeded default var (OS=tobyOS) was wrong
 *   6  ExpandEnvironmentStrings didn't expand %VAR% references
 *  16  ALL PASSED
 */

#include <windows.h>

int main(void) {
    char *cl = GetCommandLineA();
    if (!cl || !cl[0]) return 2;

    if (!SetEnvironmentVariableA("C16TEST", "hello-c16c")) return 3;

    char buf[64];
    DWORD n = GetEnvironmentVariableA("C16TEST", buf, sizeof(buf));
    if (n == 0 || lstrcmpA(buf, "hello-c16c") != 0) return 4;

    char os[64];
    GetEnvironmentVariableA("OS", os, sizeof(os));
    if (lstrcmpA(os, "tobyOS") != 0) return 5;

    char exp[128];
    ExpandEnvironmentStringsA("OS=%OS% V=%C16TEST%", exp, sizeof(exp));
    if (lstrcmpA(exp, "OS=tobyOS V=hello-c16c") != 0) return 6;

    return 16;
}
