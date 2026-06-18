/* main.c -- a STOCK, INTERACTIVE Win32 GUI .exe (Track C / C8). Built with a
 * plain `clang main.c -lgdi32 -luser32 -Wl,--subsystem,windows`. It is a
 * textbook Win32 GUI program -- RegisterClass + CreateWindowEx + a GetMessage /
 * DispatchMessage loop -- but unlike the C7 demo it actually RESPONDS to input:
 *
 *   - WM_PAINT          draws the window. Background is blue until the first
 *                       click, then GREEN -- so a screenshot proves the click
 *                       was delivered to (and handled by) the WndProc.
 *   - WM_LBUTTONDOWN    sets g_clicked and InvalidateRect()s to force a repaint
 *                       in the new colour.
 *   - WM_CLOSE          starts the idiomatic teardown: DestroyWindow(h).
 *   - WM_DESTROY        PostQuitMessage(0) -- ends the message loop.
 *
 * tobyOS delivers real mouse/keyboard/close events into GetMessage as WM_*
 * (translated from struct gui_event), routes DispatchMessage to this WndProc
 * through the user-mode trampoline, and honours the CreateSolidBrush colour in
 * FillRect -- so the whole input + draw + teardown chain is exercised.
 *
 * The exit code encodes the full proof: 8 iff the window PAINTED, a click was
 * HANDLED (WM_LBUTTONDOWN), and the close chain ran to WM_DESTROY. Anything
 * less returns 2 (the WndProc/loop ran but a step was missed).
 */

#include <windows.h>

static int g_painted   = 0;
static int g_clicked   = 0;
static int g_destroyed = 0;

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r = { 0, 0, 400, 200 };
        /* COLORREF is 0x00BBGGRR: blue before a click, green after. */
        HBRUSH b = CreateSolidBrush(g_clicked ? RGB(40, 180, 90)
                                              : RGB(40, 90, 160));
        FillRect(dc, &r, b);
        DeleteObject(b);
        TextOutA(dc, 36, 80,
                 g_clicked ? "Clicked! green now -- close me (X)"
                           : "Click anywhere on this Win32 window",
                 g_clicked ? 33 : 35);
        EndPaint(h, &ps);
        g_painted = 1;
        return 0;
    }
    case WM_LBUTTONDOWN:
        g_clicked = 1;
        InvalidateRect(h, NULL, TRUE);   /* request a repaint in the new colour */
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);                /* WM_CLOSE -> WM_DESTROY */
        return 0;
    case WM_DESTROY:
        g_destroyed = 1;
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
    wc.lpszClassName = "tobycls8";
    RegisterClassA(&wc);

    HWND h = CreateWindowExA(0, "tobycls8", "tobyOS Win32 GUI (interactive)",
                             WS_OVERLAPPEDWINDOW, 160, 140, 400, 200,
                             0, 0, hi, 0);
    if (!h) return 1;
    ShowWindow(h, sh);
    UpdateWindow(h);

    MSG msg;
    while (GetMessageA(&msg, 0, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return (g_painted && g_clicked && g_destroyed) ? 8 : 2;
}
