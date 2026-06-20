/* main.c -- a STOCK Win32 GUI .exe exercising GDI fonts + text metrics
 * (Track C / C13): CreateFontA at several sizes, SelectObject(font),
 * SetTextColor, GetTextExtentPoint32A for layout, TextOutA (scaled), and
 * DrawTextA with DT_CENTER. tobyOS maps the font onto its scalable bitmap font.
 *
 * Exit code: 13 iff the paint ran (the headline is a screenshot of multi-size,
 * multi-colour text); 2 otherwise.
 */

#include <windows.h>
#include <string.h>

static int  g_painted = 0;
static HWND g_hwnd;

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT full = { 0, 0, 440, 270 };
        HBRUSH bb = CreateSolidBrush(RGB(25, 30, 45));
        FillRect(dc, &full, bb);
        DeleteObject(bb);
        SetBkColor(dc, RGB(25, 30, 45));

        static const char *labels[4] = { "Tiny font 8", "Small font 16",
                                          "Medium font 24", "Large font 32" };
        int      sizes[4] = { 8, 16, 24, 32 };
        COLORREF cols[4]  = { RGB(255, 90, 90), RGB(90, 230, 120),
                              RGB(120, 180, 255), RGB(255, 215, 80) };
        int y = 16;
        for (int i = 0; i < 4; i++) {
            HFONT f   = CreateFontA(sizes[i], 0, 0, 0, 400, 0, 0, 0,
                                    0, 0, 0, 0, 0, "");
            HFONT old = (HFONT)SelectObject(dc, f);
            SetTextColor(dc, cols[i]);
            int len = (int)strlen(labels[i]);
            SIZE sz;
            GetTextExtentPoint32A(dc, labels[i], len, &sz);   /* measure */
            TextOutA(dc, 20, y, labels[i], len);
            y += sz.cy + 6;                                   /* lay out by metric */
            SelectObject(dc, old);
            DeleteObject(f);
        }

        /* DrawText centred in a rect with a fresh font. */
        HFONT f2 = CreateFontA(16, 0, 0, 0, 400, 0, 0, 0, 0, 0, 0, 0, 0, "");
        SelectObject(dc, f2);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT r = { 20, 224, 420, 256 };
        DrawTextA(dc, "DrawText centred in a rect", -1, &r,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        DeleteObject(f2);

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
    wc.lpszClassName = "tobyfont13";
    RegisterClassA(&wc);

    g_hwnd = CreateWindowExA(0, "tobyfont13", "tobyOS C13 -- GDI fonts",
                             WS_OVERLAPPEDWINDOW, 120, 110, 440, 270, 0, 0, hi, 0);
    if (!g_hwnd) return 1;
    ShowWindow(g_hwnd, sh);
    UpdateWindow(g_hwnd);

    MSG msg;
    int slept = 0;
    while (GetMessageA(&msg, 0, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
        if (g_painted && !slept) { Sleep(4000); slept = 1; DestroyWindow(g_hwnd); }
    }
    return g_painted ? 13 : 2;
}
