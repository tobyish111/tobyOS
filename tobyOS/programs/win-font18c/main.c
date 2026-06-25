/* main.c -- a STOCK Win32 GUI .exe proving BOLD / ITALIC TrueType faces
 * (Track C / C18c). It draws the SAME string at the SAME pixel height in four
 * faces -- Regular, Bold, Italic, Bold+Italic -- selected purely via
 * CreateFontA's lfWeight (>=700 = bold) and lfItalic. tobyOS picks the matching
 * Lato face (Regular/Bold/Italic/BoldItalic, all OFL) in the kernel rasterizer.
 *
 * Two independent proofs:
 *   - The kernel harness reads back the rendered pixels and verifies the BOLD
 *     line has measurably MORE ink than Regular (bold genuinely heavier) and the
 *     ITALIC line SLANTS (its glyph tops sit right of their bottoms). Lato Bold
 *     shares Regular's advance widths, so heaviness can only be proven by ink --
 *     a width check would be fooled.
 *   - This app additionally checks (app-side) that the Italic and BoldItalic
 *     advance widths differ from Regular/Bold (those faces DO differ in metrics),
 *     proving the distinct face files loaded; it returns 18 iff so.
 *
 * Exit code: 2 = CreateWindow failed; 3 = faces not distinct; 18 = PASS.
 *
 * Layout constants (LX/Y0/DY/PX, client WIN_W x WIN_H) are mirrored by the
 * kernel harness so it knows where each line was drawn.
 */

#include <windows.h>

#define WIN_W 600
#define WIN_H 320
#define LX    20      /* left x of every line                */
#define Y0    20      /* top y of the first line (text box)  */
#define DY    60      /* vertical spacing between lines       */
#define PX    40      /* pixel height of every face           */
#define BG    RGB(20, 24, 40)
#define FG    RGB(235, 235, 235)

static int g_painted  = 0;
static int g_exitcode = 3;

static HFONT mkfont(int weight, int italic) {
    return CreateFontA(PX, 0, 0, 0, weight, italic, 0, 0,
                       ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       DEFAULT_QUALITY, DEFAULT_PITCH, "Lato");
}

static void paint(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);

    RECT full = { 0, 0, WIN_W, WIN_H };
    HBRUSH bg = CreateSolidBrush(BG);
    FillRect(dc, &full, bg);
    DeleteObject(bg);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, FG);

    const char *s = "Typeface Hjkltb 0123";
    int len = (int)lstrlenA(s);

    struct { int weight, italic; } faces[4] = {
        { 400, 0 },   /* Regular     */
        { 700, 0 },   /* Bold        */
        { 400, 1 },   /* Italic      */
        { 700, 1 },   /* Bold Italic */
    };
    SIZE sz[4];
    for (int i = 0; i < 4; i++) {
        HFONT f   = mkfont(faces[i].weight, faces[i].italic);
        HFONT old = (HFONT)SelectObject(dc, f);
        TextOutA(dc, LX, Y0 + i * DY, s, len);
        GetTextExtentPoint32A(dc, s, len, &sz[i]);
        SelectObject(dc, old);
        DeleteObject(f);
    }

    /* App-side check: Italic and BoldItalic Lato have different advance widths
     * than Regular and Bold (Bold shares Regular's widths, so we don't compare
     * those). Distinct widths => the distinct face files genuinely loaded. */
    if (sz[2].cx != sz[0].cx && sz[3].cx != sz[1].cx)
        g_exitcode = 18;

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
    wc.lpszClassName = "tobyc18c";
    RegisterClassA(&wc);

    HWND hMain = CreateWindowExA(0, "tobyc18c", "tobyOS C18c -- bold + italic TTF",
                                 WS_OVERLAPPEDWINDOW, 70, 70, WIN_W, WIN_H, 0, 0, hi, 0);
    if (!hMain) return 2;
    ShowWindow(hMain, sh);
    UpdateWindow(hMain);

    MSG msg;
    int slept = 0;
    while (GetMessageA(&msg, 0, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
        if (g_painted && !slept) { Sleep(8000); slept = 1; DestroyWindow(hMain); }
    }
    return g_exitcode;
}
