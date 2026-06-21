/* main.c -- a STOCK Win32 GUI .exe using the common Open file dialog (Track C /
 * C16b). It creates a known file C:\c16test.txt, then on a button click calls
 * GetOpenFileNameA; tobyOS shows a real modal file picker over /data, the user
 * picks the file, and the app opens the returned path + verifies the contents.
 *
 * Exit code:
 *   2  CreateWindow failed
 *   3  GetOpenFileNameA returned FALSE (cancelled) or the picked file didn't read back
 *  16  the picked path opened and contained "C16B-OK"
 */

#include <windows.h>

#define ID_OPEN 100
static int g_result = 0, g_done = 0;

static void write_known_file(void) {
    HANDLE h = CreateFileA("C:\\c16test.txt", GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        WriteFile(h, "C16B-OK", 8, &wrote, NULL);   /* 7 chars + NUL */
        CloseHandle(h);
    }
}

static void do_open(HWND h) {
    char file[260];
    const char *def = "c16test.txt";
    int i = 0; for (; def[i]; i++) file[i] = def[i]; file[i] = 0;

    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = h;
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = sizeof(file);
    ofn.lpstrTitle  = "Pick a file (C16b)";
    ofn.lpstrFilter = "Text\0*.txt\0All\0*.*\0";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (GetOpenFileNameA(&ofn)) {
        HANDLE hf = CreateFileA(file, GENERIC_READ, 0, NULL, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf != INVALID_HANDLE_VALUE) {
            char buf[16]; DWORD got = 0;
            ReadFile(hf, buf, 8, &got, NULL);
            CloseHandle(hf);
            if (got >= 7) { buf[got < 16 ? got : 15] = 0;
                if (lstrcmpA(buf, "C16B-OK") == 0) g_result = 1; }
        }
    }
    g_done = 1;
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT full = { 0, 0, 360, 200 };
        HBRUSH bb = CreateSolidBrush(RGB(40, 48, 68));
        FillRect(dc, &full, bb); DeleteObject(bb);
        SetTextColor(dc, RGB(235, 235, 245)); SetBkMode(dc, TRANSPARENT);
        TextOutA(dc, 24, 100, "Click the button to open a file.", 32);
        EndPaint(h, &ps); return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_OPEN) do_open(h);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE pi, LPSTR cl, int sh) {
    (void)pi; (void)cl;
    write_known_file();

    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hi;
    wc.lpszClassName = "tobyc16b";
    RegisterClassA(&wc);

    HWND hMain = CreateWindowExA(0, "tobyc16b", "tobyOS C16b -- file dialog",
                                 WS_OVERLAPPEDWINDOW, 110, 110, 360, 200, 0, 0, hi, 0);
    if (!hMain) return 2;
    CreateWindowExA(0, "BUTTON", "Open File...", WS_CHILD | WS_VISIBLE,
                    30, 40, 160, 32, hMain, (HMENU)(LONG_PTR)ID_OPEN, hi, 0);
    ShowWindow(hMain, sh);
    UpdateWindow(hMain);

    MSG msg; int slept = 0;
    while (GetMessageA(&msg, 0, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
        if (g_done && !slept) { Sleep(1500); slept = 1; DestroyWindow(hMain); }
    }
    return g_result ? 16 : 3;
}
