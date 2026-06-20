/* main.c -- a STOCK Win32 GUI .exe exercising MORE GDI + a CHILD/OWNED window
 * (Track C / C11). Built with a plain clang + -lgdi32 -luser32.
 *
 * The MAIN window's WndProc draws richer GDI than the FillRect/TextOut of C7-C10:
 * a pen-drawn line (MoveToEx/LineTo), a Rectangle, and an Ellipse. It also opens
 * a CHILD window (created with hWndParent = the main window -- reachable now that
 * tobyOS's marshalling gate passes 10 args). The app then destroys ONLY the main
 * window; tobyOS cascades the destroy to the child (parent->child lifetime), so
 * BOTH WndProcs receive WM_DESTROY from the single DestroyWindow(hMain) call.
 *
 * Exit code (distinct per step):
 *   2  CreateWindow returned NULL for the main or child window
 *   3  the loop ended but the main or child never painted
 *   4  the main window's WM_DESTROY didn't run
 *   5  the CHILD's WM_DESTROY didn't run -- i.e. destroying the parent did NOT
 *      cascade to the child (the child/owned relationship failed)
 *  11  ALL PASSED
 */

#include <windows.h>

static HWND hMain, hChild;
static int paintedMain, paintedChild, destroyedMain, destroyedChild;
static int nlive, phase;

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    int isMain = (h == hMain), isChild = (h == hChild);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (isMain) {
            RECT bg = { 0, 0, 400, 260 };
            HBRUSH bb = CreateSolidBrush(RGB(30, 40, 70));
            FillRect(dc, &bg, bb);
            DeleteObject(bb);
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 210, 40));
            HPEN old = (HPEN)SelectObject(dc, pen);
            MoveToEx(dc, 30, 40, NULL);             /* a pen-drawn line */
            LineTo(dc, 360, 40);
            Rectangle(dc, 40, 70, 200, 170);        /* a rectangle */
            Ellipse(dc, 220, 70, 360, 170);         /* an ellipse */
            SelectObject(dc, old);
            DeleteObject(pen);
            TextOutA(dc, 30, 210, "GDI: line + rectangle + ellipse", 31);
            paintedMain = 1;
        } else if (isChild) {
            RECT bg = { 0, 0, 240, 120 };
            HBRUSH bb = CreateSolidBrush(RGB(60, 110, 60));
            FillRect(dc, &bg, bb);
            DeleteObject(bb);
            TextOutA(dc, 24, 50, "child window (owned by main)", 28);
            paintedChild = 1;
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY:
        if (isMain)  destroyedMain = 1;
        if (isChild) destroyedChild = 1;
        if (--nlive == 0) PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE pi, LPSTR cl, int sh) {
    (void)pi; (void)cl;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hi;
    wc.lpszClassName = "tobycls11";
    RegisterClassA(&wc);

    hMain = CreateWindowExA(0, "tobycls11", "tobyOS C11 -- GDI (main)",
                            WS_OVERLAPPEDWINDOW, 120, 110, 400, 260,
                            0, 0, hi, 0);
    /* The child carries hWndParent = hMain (its 9th arg -- reachable via the
     * 10-arg gate). */
    hChild = CreateWindowExA(0, "tobycls11", "child",
                             WS_CHILD | WS_VISIBLE, 560, 320, 240, 120,
                             hMain, 0, hi, 0);
    if (!hMain || !hChild) return 2;
    nlive = 2;
    ShowWindow(hMain, sh);  ShowWindow(hChild, sh);
    UpdateWindow(hMain);    UpdateWindow(hChild);

    MSG msg;
    while (GetMessageA(&msg, 0, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
        /* Once both have drawn (and a screenshot can catch them), destroy ONLY
         * the main window -- the child must go with it. */
        if (paintedMain && paintedChild && phase == 0) {
            Sleep(4000);
            DestroyWindow(hMain);
            phase = 1;
        }
    }

    if (!(paintedMain && paintedChild)) return 3;
    if (!destroyedMain)                 return 4;
    if (!destroyedChild)                return 5;   /* parent->child cascade */
    return 11;
}
