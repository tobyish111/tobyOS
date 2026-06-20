/* main.c -- a STOCK Win32 GUI .exe with TWO top-level windows (Track C / C10).
 * Built with a plain `clang main.c -lgdi32 -luser32 -Wl,--subsystem,windows`.
 *
 * It registers one window class and creates TWO windows (A and B) that share a
 * single WndProc which discriminates by HWND -- the idiomatic multi-window
 * pattern. tobyOS now keeps a per-window state table and routes each window's
 * WM_PAINT / input / close independently, and stashes the right WndProc per
 * message so the C7 DispatchMessage trampoline calls it.
 *
 * The exit code proves independent multi-window handling (distinct codes per
 * step for debuggability):
 *   2  CreateWindow returned NULL for A or B
 *   3  the message loop ended but not every window painted
 *   4  more (or fewer) than exactly ONE window received the click
 *      (proves the click was routed to a single window, not broadcast)
 *   5  not both windows ran their WM_DESTROY (independent close)
 *  10  ALL PASSED
 */

#include <windows.h>

static HWND hA, hB;
static int paintedA, paintedB, clickedA, clickedB, destroyedA, destroyedB;
static int nlive;

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    int isA = (h == hA), isB = (h == hB);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r = { 0, 0, 360, 180 };
        int clicked = isA ? clickedA : clickedB;
        HBRUSH b = CreateSolidBrush(clicked ? RGB(40, 180, 90)   /* green */
                                            : RGB(40, 90, 160));  /* blue  */
        FillRect(dc, &r, b);
        DeleteObject(b);
        TextOutA(dc, 28, 78, isA ? "Window A -- click / close me"
                                 : "Window B -- click / close me", 28);
        EndPaint(h, &ps);
        if (isA) paintedA = 1;
        if (isB) paintedB = 1;
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (isA) clickedA = 1;
        if (isB) clickedB = 1;
        InvalidateRect(h, NULL, TRUE);
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        if (isA) destroyedA = 1;
        if (isB) destroyedB = 1;
        if (--nlive == 0) PostQuitMessage(0);   /* quit only when both are gone */
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE pi, LPSTR cl, int sh) {
    (void)pi; (void)cl;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hi;
    wc.lpszClassName = "tobycls10";
    RegisterClassA(&wc);

    hA = CreateWindowExA(0, "tobycls10", "tobyOS Win32 -- Window A",
                         WS_OVERLAPPEDWINDOW, 120, 120, 360, 180, 0, 0, hi, 0);
    hB = CreateWindowExA(0, "tobycls10", "tobyOS Win32 -- Window B",
                         WS_OVERLAPPEDWINDOW, 560, 320, 360, 180, 0, 0, hi, 0);
    if (!hA || !hB) return 2;
    nlive = 2;
    ShowWindow(hA, sh); ShowWindow(hB, sh);
    UpdateWindow(hA);   UpdateWindow(hB);

    MSG msg;
    while (GetMessageA(&msg, 0, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    if (!(paintedA && paintedB))            return 3;
    if ((clickedA + clickedB) != 1)         return 4;
    if (!(destroyedA && destroyedB))        return 5;
    return 10;
}
