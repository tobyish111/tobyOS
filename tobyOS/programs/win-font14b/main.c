/* main.c -- a STOCK Win32 GUI .exe that renders REAL TrueType text (Track C /
 * C14b). It CreateFontA's several pixel heights, SelectObject + SetTextColor +
 * TextOutA's real strings, DrawTextA-centres a line, and measures text with
 * GetTextExtentPoint32A. tobyOS rasterizes genuine Lato (OFL) glyphs in the
 * kernel via stb_truetype.
 *
 * Exit code:
 *   2  CreateWindow failed
 *   3  text measured as MONOSPACE (the 8x8 bitmap fallback, not TTF)
 *  14  TTF proven: a proportional font ("WWWW" wider than "iiii")
 */

#include <windows.h>

static int g_painted = 0;
static int g_proportional = 0;

static void paint(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);

    RECT full = { 0, 0, 640, 360 };
    HBRUSH bg = CreateSolidBrush(RGB(28, 32, 48));
    FillRect(dc, &full, bg);
    DeleteObject(bg);

    struct { int px; COLORREF col; const char *s; } rows[] = {
        { 18, RGB(240, 240, 240), "TrueType 18px - The quick brown fox" },
        { 26, RGB(120, 200, 255), "TrueType 26px - jumps over 0123456789" },
        { 36, RGB(160, 240, 160), "Real glyphs @ 36px" },
        { 52, RGB(255, 200, 120), "Big 52px AaWwIi" },
    };
    int y = 16;
    for (int i = 0; i < 4; i++) {
        HFONT f = CreateFontA(rows[i].px, 0, 0, 0, 400, 0, 0, 0,
                              ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              DEFAULT_QUALITY, DEFAULT_PITCH, "Lato");
        HFONT old = (HFONT)SelectObject(dc, f);
        SetTextColor(dc, rows[i].col);
        SetBkMode(dc, TRANSPARENT);
        TextOutA(dc, 20, y, rows[i].s, (int)lstrlenA(rows[i].s));
        SelectObject(dc, old);
        DeleteObject(f);
        y += rows[i].px + 8;
    }

    /* Measure to prove proportional (TTF) vs monospace (bitmap fallback). */
    HFONT fm = CreateFontA(32, 0, 0, 0, 400, 0, 0, 0, ANSI_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           DEFAULT_QUALITY, DEFAULT_PITCH, "Lato");
    HFONT oldm = (HFONT)SelectObject(dc, fm);
    SIZE szW, szI;
    GetTextExtentPoint32A(dc, "WWWW", 4, &szW);
    GetTextExtentPoint32A(dc, "iiii", 4, &szI);
    if (szW.cx > szI.cx + 8) g_proportional = 1;

    /* Centred DrawText in the bottom strip. */
    SetTextColor(dc, RGB(255, 255, 255));
    RECT r = { 20, 300, 620, 344 };
    DrawTextA(dc, "Centered DrawText - tobyOS C14b", -1, &r, DT_CENTER | DT_VCENTER);
    SelectObject(dc, oldm);
    DeleteObject(fm);

    EndPaint(h, &ps);
    g_painted = 1;
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT:   paint(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE pi, LPSTR cl, int sh) {
    (void)pi; (void)cl;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hi;
    wc.lpszClassName = "tobyc14b";
    RegisterClassA(&wc);

    HWND hMain = CreateWindowExA(0, "tobyc14b", "tobyOS C14b -- TrueType fonts",
                                 WS_OVERLAPPEDWINDOW, 90, 90, 640, 360, 0, 0, hi, 0);
    if (!hMain) return 2;
    ShowWindow(hMain, sh);
    UpdateWindow(hMain);

    MSG msg;
    int slept = 0;
    while (GetMessageA(&msg, 0, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
        if (g_painted && !slept) { Sleep(7000); slept = 1; DestroyWindow(hMain); }
    }
    return g_proportional ? 14 : 3;
}
