/* main.c -- a STOCK Win32 GUI .exe using REAL system controls: a BUTTON and an
 * EDIT (Track C / C12). Built with a plain clang + -lgdi32 -luser32.
 *
 * The app creates the controls with the predefined "EDIT"/"BUTTON" classes and
 * a parent window -- it never implements them. tobyOS draws each control and
 * handles its input: typing into the edit stores text; clicking the button
 * sends WM_COMMAND(BN_CLICKED) to the parent. The WndProc, on WM_COMMAND, reads
 * the edit text back via GetWindowTextA.
 *
 * Exit code (distinct per step):
 *   2  CreateWindow returned NULL for the main window or a control
 *   3  the loop ended but the button was never clicked (no WM_COMMAND)
 *   4  the button was clicked but the EDIT had no text (edit input/readback failed)
 *  12  ALL PASSED
 */

#include <windows.h>

#define ID_EDIT 201
#define ID_BTN  202

static HWND hMain, hEdit, hBtn;
static int  g_clicked = 0, g_haveText = 0, g_done = 0;

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT full = { 0, 0, 380, 220 };
        HBRUSH bb = CreateSolidBrush(RGB(45, 55, 75));
        FillRect(dc, &full, bb);
        DeleteObject(bb);
        TextOutA(dc, 30, 22, "Type in the edit, then click the button:", 40);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_BTN) {
            g_clicked = 1;
            char buf[40] = {0};
            int n = GetWindowTextA(hEdit, buf, sizeof(buf));
            if (n > 0 && buf[0]) g_haveText = 1;
            g_done = 1;
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
    wc.lpszClassName = "tobyctrl12";
    RegisterClassA(&wc);

    hMain = CreateWindowExA(0, "tobyctrl12", "tobyOS C12 -- controls",
                            WS_OVERLAPPEDWINDOW, 130, 120, 380, 220,
                            0, 0, hi, 0);
    /* The EDIT is created first so it takes initial keyboard focus. */
    hEdit = CreateWindowExA(0, "EDIT", "", WS_CHILD | WS_VISIBLE,
                            30, 60, 220, 26, hMain, (HMENU)(LONG_PTR)ID_EDIT, hi, 0);
    hBtn  = CreateWindowExA(0, "BUTTON", "Click Me", WS_CHILD | WS_VISIBLE,
                            30, 110, 120, 32, hMain, (HMENU)(LONG_PTR)ID_BTN, hi, 0);
    if (!hMain || !hEdit || !hBtn) return 2;
    ShowWindow(hMain, sh);
    ShowWindow(hEdit, sh);
    ShowWindow(hBtn, sh);
    UpdateWindow(hMain);

    MSG msg;
    int slept = 0;
    while (GetMessageA(&msg, 0, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
        if (g_done && !slept) { Sleep(3000); slept = 1; DestroyWindow(hMain); }
    }

    if (!g_clicked)  return 3;
    if (!g_haveText) return 4;
    return 12;
}
