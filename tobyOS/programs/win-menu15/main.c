/* main.c -- a STOCK Win32 GUI .exe exercising the C15 additions: a real MENU BAR
 * (File/Help with dropdowns), a TIMER (periodic WM_TIMER updating a counter), and
 * a multi-button MessageBox (MB_YESNO). tobyOS draws + input-handles the menu and
 * synthesises WM_TIMER; the app gets WM_COMMAND from menu clicks.
 *
 * Exit code:
 *   2  CreateWindow failed
 *   3  the timer never fired (WM_TIMER)
 *   4  the About MessageBox didn't return IDYES
 *  15  ALL PASSED (menu command + timer + Yes/No box)
 */

#include <windows.h>

#define ID_NEW   101
#define ID_OPEN  102
#define ID_EXIT  103
#define ID_ABOUT 201

static int g_ticks = 0, g_about = 0, g_menucmd = 0;

/* tiny unsigned int -> decimal (no CRT formatting). */
static void uitoa(unsigned v, char *out) {
    char tmp[16]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    int o = 0;
    while (n) out[o++] = tmp[--n];
    out[o] = 0;
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT full = { 0, 0, 460, 300 };
        HBRUSH bb = CreateSolidBrush(RGB(36, 44, 66));
        FillRect(dc, &full, bb);
        DeleteObject(bb);
        SetTextColor(dc, RGB(235, 235, 245));
        SetBkMode(dc, TRANSPARENT);
        TextOutA(dc, 20, 48, "Use the File / Help menu above.", 31);
        char line[32], num[16];
        uitoa((unsigned)g_ticks, num);
        int n = 0;
        const char *pre = "Timer ticks: ";
        for (const char *p = pre; *p; p++) line[n++] = *p;
        for (char *p = num; *p; p++) line[n++] = *p;
        line[n] = 0;
        SetTextColor(dc, RGB(150, 220, 150));
        TextOutA(dc, 20, 84, line, n);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TIMER:
        g_ticks++;
        InvalidateRect(h, NULL, TRUE);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_ABOUT:
            g_menucmd = 1;
            if (MessageBoxA(h, "Real menus + timers + a Yes/No box.\nClick Yes.",
                            "About tobyOS C15", MB_YESNO) == IDYES)
                g_about = 1;
            return 0;
        case ID_EXIT:
            DestroyWindow(h);
            return 0;
        case ID_NEW:  g_menucmd = 1; return 0;
        case ID_OPEN: g_menucmd = 1; return 0;
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE pi, LPSTR cl, int sh) {
    (void)pi; (void)cl;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hi;
    wc.lpszClassName = "tobyc15";
    RegisterClassA(&wc);

    HWND hMain = CreateWindowExA(0, "tobyc15", "tobyOS C15 -- menus + timers",
                                 WS_OVERLAPPEDWINDOW, 100, 100, 460, 300, 0, 0, hi, 0);
    if (!hMain) return 2;

    HMENU bar  = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuA(file, MF_STRING,    ID_NEW,  "New");
    AppendMenuA(file, MF_STRING,    ID_OPEN, "Open");
    AppendMenuA(file, MF_SEPARATOR, 0,       NULL);
    AppendMenuA(file, MF_STRING,    ID_EXIT, "Exit");
    HMENU help = CreatePopupMenu();
    AppendMenuA(help, MF_STRING,    ID_ABOUT, "About");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)file, "File");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)help, "Help");
    SetMenu(hMain, bar);

    SetTimer(hMain, 1, 500, NULL);

    ShowWindow(hMain, sh);
    UpdateWindow(hMain);

    MSG msg;
    while (GetMessageA(&msg, 0, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    KillTimer(hMain, 1);

    if (g_ticks < 2) return 3;
    if (!g_about)    return 4;
    return 15;
}
