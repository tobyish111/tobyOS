/* main.c -- Track C / C23: proves tobyOS's wide-char (UTF-16) Windows support:
 * the wide CRT string family, the wide printf family (wprintf/swprintf), wide
 * stdio output (fputws/_putws), and %ls in the *narrow* printf engine.
 *
 * Built like win-crt/win-printf21 (clang --target mingw,
 * -D__USE_MINGW_ANSI_STDIO=0) so the wprintf/swprintf family inlines to the
 * ucrt wide stdio primitives __stdio_common_vfwprintf / __stdio_common_vswprintf,
 * which tobyOS shims kernel-side (win32_wvformat / transcode -> win32_format_core).
 *
 * real_main returns 23 ONLY if every wide-string operation produced the exact
 * expected result; any mismatch returns a distinct 9x code that pinpoints the
 * failing step. Wide output is also echoed to the serial log for eyeballing.
 */

#include <stdio.h>
#include <wchar.h>

__declspec(dllimport) __attribute__((noreturn))
void __stdcall ExitProcess(unsigned int code);

static int real_main(void) {
    wchar_t buf[64];

    /* wcscpy + wcscat: build L"Hello, wide" piecewise. */
    wcscpy(buf, L"Hello");
    wcscat(buf, L", ");
    wcscat(buf, L"wide");
    if (wcslen(buf) != 11)                 return 91;
    if (wcscmp(buf, L"Hello, wide") != 0)  return 92;
    if (wcsncmp(buf, L"Hello", 5) != 0)    return 93;

    /* wcschr / wcsrchr: locate code units. */
    wchar_t *comma = wcschr(buf, L',');
    if (!comma || comma != buf + 5)        return 94;
    wchar_t *lastL = wcsrchr(buf, L'l');   /* "Hello, wide": last 'l' at idx 3 */
    if (!lastL || lastL != buf + 3)        return 95;

    /* swprintf into a wide buffer, then verify the rendered code units. */
    wchar_t sb[64];
    swprintf(sb, 64, L"%ls=%d", L"val", 42);
    if (wcscmp(sb, L"val=42") != 0)        return 96;

    /* wide formatted output (wprintf): %ls wide string, %d, wide %c. */
    wprintf(L"C23W|%ls|len=%d|%c|\n", buf, (int)wcslen(buf), L'X');

    /* the NARROW printf engine must also accept a wide string via %ls. */
    printf("C23N|%ls|\n", buf);

    /* wide stdio line emitters. */
    fputws(L"C23|fputws-ok\n", stdout);
    _putws(L"C23|putws-ok");

    return 23;
}

void __attribute__((force_align_arg_pointer)) c23_entry(void) {
    ExitProcess((unsigned)real_main());
}
