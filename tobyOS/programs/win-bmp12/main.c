/* main.c -- a STOCK Win32 GUI .exe that composes an in-memory bitmap and
 * BitBlts it onto its window (Track C / C12). Built with a plain clang +
 * -lgdi32 -luser32. Proves CreateCompatibleDC + CreateBitmap + SelectObject +
 * BitBlt onto tobyOS's compositor (gui_window_blit).
 *
 * Exit code: 12 iff the BitBlt path ran in WM_PAINT (the headline is a
 * screenshot of the gradient bitmap on the window); 2 otherwise.
 */

#include <windows.h>

#define BMP_W 200
#define BMP_H 130
static unsigned int g_pix[BMP_W * BMP_H];   /* 0x00RRGGBB == tobyOS XRGB */
static int g_blit = 0;
static HWND g_hwnd;

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT full = { 0, 0, 380, 230 };
        HBRUSH bb = CreateSolidBrush(RGB(18, 20, 30));
        FillRect(dc, &full, bb);
        DeleteObject(bb);

        /* Build a red-x / green-y gradient bitmap, then BitBlt it. */
        for (int y = 0; y < BMP_H; y++)
            for (int x = 0; x < BMP_W; x++) {
                unsigned int r = (unsigned)(x * 255 / BMP_W);
                unsigned int g = (unsigned)(y * 255 / BMP_H);
                g_pix[y * BMP_W + x] = (r << 16) | (g << 8) | 0x40;
            }
        HDC     mdc = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateBitmap(BMP_W, BMP_H, 1, 32, g_pix);
        SelectObject(mdc, bmp);
        BitBlt(dc, 90, 60, BMP_W, BMP_H, mdc, 0, 0, SRCCOPY);
        DeleteObject(bmp);
        DeleteDC(mdc);

        TextOutA(dc, 110, 30, "BitBlt: an in-memory bitmap", 27);
        EndPaint(h, &ps);
        g_blit = 1;
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
    wc.lpszClassName = "tobybmp12";
    RegisterClassA(&wc);

    g_hwnd = CreateWindowExA(0, "tobybmp12", "tobyOS C12 -- BitBlt",
                             WS_OVERLAPPEDWINDOW, 130, 120, 380, 230,
                             0, 0, hi, 0);
    if (!g_hwnd) return 1;
    ShowWindow(g_hwnd, sh);
    UpdateWindow(g_hwnd);

    MSG msg;
    int slept = 0;
    while (GetMessageA(&msg, 0, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
        if (g_blit && !slept) { Sleep(4000); slept = 1; DestroyWindow(g_hwnd); }
    }
    return g_blit ? 12 : 2;
}
