/* main.c -- a STOCK Win32 GUI .exe (Track C / C7). Built with a plain
 * `clang main.c -lgdi32 -luser32 -Wl,--subsystem,windows`. It is a textbook
 * Win32 GUI program: RegisterClass + CreateWindowEx + a GetMessage /
 * DispatchMessage loop, with the actual drawing done in the window
 * procedure's WM_PAINT handler (BeginPaint -> FillRect + TextOut ->
 * EndPaint). tobyOS maps the window onto a real desktop window
 * (sys_gui_create) and the drawing onto sys_gui_fill/sys_gui_text, and --
 * the hard part -- routes DispatchMessage to the app's WndProc through a
 * user-mode trampoline (a kernel shim can't call CPL3 code).
 *
 * The WndProc draws and sets a flag; WinMain then keeps the window on screen
 * for a moment (so a screenshot can catch it) and quits, returning 7 iff the
 * WndProc actually ran -- which proves the whole user32/gdi32 bridge incl.
 * the WndProc callback.
 */

#include <windows.h>

static int g_painted = 0;

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r = { 20, 20, 360, 160 };
        FillRect(dc, &r, (HBRUSH)(COLOR_WINDOW + 1));
        TextOutA(dc, 40, 70, "Hello from a Win32 GUI app on tobyOS", 36);
        EndPaint(h, &ps);
        g_painted = 1;
        return 0;
    }
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
    wc.lpszClassName = "tobycls";
    RegisterClassA(&wc);

    HWND h = CreateWindowExA(0, "tobycls", "tobyOS Win32 GUI",
                             WS_OVERLAPPEDWINDOW, 120, 120, 400, 200,
                             0, 0, hi, 0);
    if (!h) return 1;
    ShowWindow(h, sh);
    UpdateWindow(h);

    MSG msg;
    int slept = 0;
    while (GetMessageA(&msg, 0, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
        if (g_painted && !slept) { Sleep(2000); slept = 1; PostQuitMessage(0); }
    }
    return g_painted ? 7 : 2;
}
