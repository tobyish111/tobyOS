/* main.c -- a STOCK Win32 GUI .exe exercising the C14 controls: a GROUPBOX
 * (visual frame), two mutually-exclusive RADIO buttons, a COMBOBOX (dropdown
 * edit+list), and a TAB control (SysTabControl32) whose pages switch on a tab
 * click. tobyOS draws + input-handles all of them (the app implements none);
 * the app drives state via SendMessage and gets WM_COMMAND/WM_NOTIFY.
 *
 * Exit code (distinct per step):
 *   2  a CreateWindow returned NULL
 *   3  the second RADIO ("Slow") was never selected
 *   4  the COMBOBOX selection isn't "Green" (index 1)
 *   5  the TAB selection isn't "Two" (index 1)
 *  14  ALL PASSED
 */

#include <windows.h>
#include <commctrl.h>

#define ID_RAD_FAST 401
#define ID_RAD_SLOW 402
#define ID_COMBO    403
#define ID_TAB      404
#define ID_FINISH   405
#define ID_PAGE0    501
#define ID_PAGE1    502
#define ID_PAGE2    503

static HWND hMain, hGroup, hFast, hSlow, hCombo, hTab, hFinish;
static HWND hPage[3];
static int  g_radio = 0, g_combo = -1, g_tab = -1, g_done = 0;

static void show_page(int sel) {
    for (int i = 0; i < 3; i++)
        ShowWindow(hPage[i], (i == sel) ? SW_SHOW : SW_HIDE);
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT full = { 0, 0, 480, 340 };
        HBRUSH bb = CreateSolidBrush(RGB(45, 55, 75));
        FillRect(dc, &full, bb);
        DeleteObject(bb);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == ID_RAD_FAST || id == ID_RAD_SLOW) {
            if (SendMessageA(hSlow, BM_GETCHECK, 0, 0)) g_radio = 2;
            else if (SendMessageA(hFast, BM_GETCHECK, 0, 0)) g_radio = 1;
        } else if (id == ID_COMBO && code == CBN_SELCHANGE) {
            g_combo = (int)SendMessageA(hCombo, CB_GETCURSEL, 0, 0);
        } else if (id == ID_FINISH) {
            g_done = 1;
        }
        return 0;
    }
    case WM_NOTIFY: {
        LPNMHDR nh = (LPNMHDR)lp;
        if (nh && nh->idFrom == ID_TAB && nh->code == TCN_SELCHANGE) {
            g_tab = (int)SendMessageA(hTab, TCM_GETCURSEL, 0, 0);
            show_page(g_tab);
        }
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
    wc.lpszClassName = "tobyc14";
    RegisterClassA(&wc);

    hMain = CreateWindowExA(0, "tobyc14", "tobyOS C14 -- controls",
                            WS_OVERLAPPEDWINDOW, 100, 100, 480, 340, 0, 0, hi, 0);

    hGroup = CreateWindowExA(0, "BUTTON", "Mode",
                             WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                             20, 15, 200, 75, hMain, (HMENU)(LONG_PTR)490, hi, 0);
    hFast  = CreateWindowExA(0, "BUTTON", "Fast",
                             WS_CHILD | WS_VISIBLE | WS_GROUP | BS_AUTORADIOBUTTON,
                             35, 35, 150, 20, hMain, (HMENU)(LONG_PTR)ID_RAD_FAST, hi, 0);
    hSlow  = CreateWindowExA(0, "BUTTON", "Slow",
                             WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                             35, 60, 150, 20, hMain, (HMENU)(LONG_PTR)ID_RAD_SLOW, hi, 0);

    CreateWindowExA(0, "STATIC", "Colour:", WS_CHILD | WS_VISIBLE,
                    250, 18, 80, 14, hMain, (HMENU)(LONG_PTR)491, hi, 0);
    hCombo = CreateWindowExA(0, "COMBOBOX", "",
                             WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                             250, 34, 170, 120, hMain, (HMENU)(LONG_PTR)ID_COMBO, hi, 0);

    hTab = CreateWindowExA(0, WC_TABCONTROLA, "",
                           WS_CHILD | WS_VISIBLE,
                           20, 110, 440, 180, hMain, (HMENU)(LONG_PTR)ID_TAB, hi, 0);

    hPage[0] = CreateWindowExA(0, "STATIC", "This is page One", WS_CHILD | WS_VISIBLE,
                               40, 150, 300, 16, hMain, (HMENU)(LONG_PTR)ID_PAGE0, hi, 0);
    hPage[1] = CreateWindowExA(0, "STATIC", "This is page Two", WS_CHILD,
                               40, 150, 300, 16, hMain, (HMENU)(LONG_PTR)ID_PAGE1, hi, 0);
    hPage[2] = CreateWindowExA(0, "STATIC", "This is page Three", WS_CHILD,
                               40, 150, 300, 16, hMain, (HMENU)(LONG_PTR)ID_PAGE2, hi, 0);

    hFinish = CreateWindowExA(0, "BUTTON", "Finish", WS_CHILD | WS_VISIBLE,
                              180, 300, 120, 30, hMain, (HMENU)(LONG_PTR)ID_FINISH, hi, 0);

    if (!hMain || !hGroup || !hFast || !hSlow || !hCombo || !hTab || !hFinish ||
        !hPage[0] || !hPage[1] || !hPage[2])
        return 2;

    SendMessageA(hCombo, CB_ADDSTRING, 0, (LPARAM)"Red");
    SendMessageA(hCombo, CB_ADDSTRING, 0, (LPARAM)"Green");
    SendMessageA(hCombo, CB_ADDSTRING, 0, (LPARAM)"Blue");

    TCITEMA ti; ti.mask = TCIF_TEXT;
    ti.pszText = (LPSTR)"One";   SendMessageA(hTab, TCM_INSERTITEMA, 0, (LPARAM)&ti);
    ti.pszText = (LPSTR)"Two";   SendMessageA(hTab, TCM_INSERTITEMA, 1, (LPARAM)&ti);
    ti.pszText = (LPSTR)"Three"; SendMessageA(hTab, TCM_INSERTITEMA, 2, (LPARAM)&ti);
    show_page(0);

    ShowWindow(hMain, sh);
    UpdateWindow(hMain);

    MSG msg;
    int slept = 0;
    while (GetMessageA(&msg, 0, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
        if (g_done && !slept) { Sleep(1200); slept = 1; DestroyWindow(hMain); }
    }

    if (g_radio != 2) return 3;
    if (g_combo != 1) return 4;
    if (g_tab   != 1) return 5;
    return 14;
}
