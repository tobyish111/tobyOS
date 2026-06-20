/* main.c -- a STOCK Win32 GUI .exe using more REAL system controls + a modal
 * dialog (Track C / C13): a STATIC label, a CHECKBOX, a LISTBOX, a SCROLLBAR,
 * and MessageBoxA. tobyOS draws + handles the controls (via SendMessage for
 * listbox/checkbox state); the app gets WM_COMMAND/WM_VSCROLL notifications.
 *
 * Exit code (distinct per step):
 *   2  a CreateWindow returned NULL
 *   3  the checkbox was never checked
 *   4  no listbox item was selected
 *   5  the scrollbar was never moved
 *   6  the MessageBox didn't return IDOK
 *  13  ALL PASSED
 */

#include <windows.h>

#define ID_CHK  301
#define ID_LIST 302
#define ID_SB   303
#define ID_BTN  304

static HWND hMain, hStatic, hChk, hList, hSb, hBtn;
static int  g_checked = 0, g_sel = -1, g_scroll = 0, g_msgbox = 0, g_done = 0;

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT full = { 0, 0, 420, 300 };
        HBRUSH bb = CreateSolidBrush(RGB(40, 50, 70));
        FillRect(dc, &full, bb);
        DeleteObject(bb);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == ID_CHK) {
            g_checked = (int)SendMessageA(hChk, BM_GETCHECK, 0, 0);
        } else if (id == ID_LIST && code == LBN_SELCHANGE) {
            g_sel = (int)SendMessageA(hList, LB_GETCURSEL, 0, 0);
        } else if (id == ID_BTN) {
            int r = MessageBoxA(h, "Controls all set. Click OK.", "Dialog", MB_OK);
            if (r == IDOK) g_msgbox = 1;
            g_done = 1;
        }
        return 0;
    }
    case WM_VSCROLL:
        g_scroll = (int)GetScrollPos(hSb, SB_CTL);
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
    wc.lpszClassName = "tobyc13";
    RegisterClassA(&wc);

    hMain   = CreateWindowExA(0, "tobyc13", "tobyOS C13 -- controls",
                              WS_OVERLAPPEDWINDOW, 100, 100, 420, 300, 0, 0, hi, 0);
    hStatic = CreateWindowExA(0, "STATIC", "Options:", WS_CHILD | WS_VISIBLE,
                              24, 20, 140, 16, hMain, (HMENU)(LONG_PTR)399, hi, 0);
    hChk    = CreateWindowExA(0, "BUTTON", "Enable feature",
                              WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                              24, 46, 170, 22, hMain, (HMENU)(LONG_PTR)ID_CHK, hi, 0);
    hList   = CreateWindowExA(0, "LISTBOX", "", WS_CHILD | WS_VISIBLE,
                              24, 80, 160, 90, hMain, (HMENU)(LONG_PTR)ID_LIST, hi, 0);
    hSb     = CreateWindowExA(0, "SCROLLBAR", "", WS_CHILD | WS_VISIBLE,
                              200, 80, 18, 90, hMain, (HMENU)(LONG_PTR)ID_SB, hi, 0);
    hBtn    = CreateWindowExA(0, "BUTTON", "Show dialog", WS_CHILD | WS_VISIBLE,
                              24, 200, 130, 30, hMain, (HMENU)(LONG_PTR)ID_BTN, hi, 0);
    if (!hMain || !hChk || !hList || !hSb || !hBtn) return 2;

    SendMessageA(hList, LB_ADDSTRING, 0, (LPARAM)"Apple");
    SendMessageA(hList, LB_ADDSTRING, 0, (LPARAM)"Banana");
    SendMessageA(hList, LB_ADDSTRING, 0, (LPARAM)"Cherry");
    SetScrollRange(hSb, SB_CTL, 0, 100, TRUE);

    ShowWindow(hMain, sh);   ShowWindow(hStatic, sh); ShowWindow(hChk, sh);
    ShowWindow(hList, sh);   ShowWindow(hSb, sh);     ShowWindow(hBtn, sh);
    UpdateWindow(hMain);

    MSG msg;
    int slept = 0;
    while (GetMessageA(&msg, 0, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
        if (g_done && !slept) { Sleep(2000); slept = 1; DestroyWindow(hMain); }
    }

    if (!g_checked)   return 3;
    if (g_sel < 0)    return 4;
    if (g_scroll <= 0) return 5;
    if (!g_msgbox)    return 6;
    return 13;
}
