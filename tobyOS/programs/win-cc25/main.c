/* main.c -- a STOCK Win32 GUI .exe exercising the C25 comctl32 COMMON CONTROLS:
 * a report-view SysListView32 (columns + rows + subitems + selection), a
 * SysTreeView32 (root/child/grandchild nodes, expand, selection), a multi-part
 * msctls_statusbar32, an msctls_progress32, and a ToolbarWindow32. tobyOS's
 * comctl32 layer draws + stores all of them; the app drives them purely through
 * the real comctl32 API (InitCommonControlsEx + LVM_, TVM_, SB_, PBM_, TB_ msgs),
 * then READS BACK every value to prove the round-trip -- so a PASS proves the
 * control state really lives in the kernel, not that the binary merely started.
 *
 * Exit code (distinct per failed check; 25 == all passed):
 *   2  a CreateWindow returned NULL
 *   3  ListView item count != 4
 *   4  ListView item text round-trip failed
 *   5  ListView subitem text round-trip failed
 *   6  ListView selection (LVM_SETITEMSTATE/GETNEXTITEM) failed
 *   7  TreeView node count != 5
 *   8  TreeView root lookup failed
 *   9  TreeView caret (select) round-trip failed
 *  10  Progress GetPos != 60 after SetPos
 *  11  Progress StepIt didn't advance by the step
 *  12  StatusBar text round-trip failed
 *  13  Toolbar button count != 3
 *  25  ALL PASSED
 */

#include <windows.h>
#include <commctrl.h>
#include <string.h>

#define ID_LIST  601
#define ID_TREE  602
#define ID_STAT  603
#define ID_PROG  604
#define ID_TOOL  605

static HWND hMain, hList, hTree, hStatus, hProg, hTool;
static int  g_rc = 25;
static int  g_done = 0;

/* Build the controls' content and validate every value via read-back. */
static int build_and_check(HINSTANCE hi) {
    /* ---- ListView (report view): 3 columns, 4 rows, subitems ---- */
    LVCOLUMNA col; memset(&col, 0, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.cx = 120; col.pszText = (LPSTR)"Name";   col.iSubItem = 0;
    SendMessageA(hList, LVM_INSERTCOLUMNA, 0, (LPARAM)&col);
    col.cx = 70;  col.pszText = (LPSTR)"Code";   col.iSubItem = 1;
    SendMessageA(hList, LVM_INSERTCOLUMNA, 1, (LPARAM)&col);
    col.cx = 90;  col.pszText = (LPSTR)"Status"; col.iSubItem = 2;
    SendMessageA(hList, LVM_INSERTCOLUMNA, 2, (LPARAM)&col);

    static const char *names[4]  = { "Alpha", "Bravo", "Charlie", "Delta" };
    static const char *codes[4]  = { "A-01", "B-12", "C-23", "D-34" };
    static const char *states[4] = { "OK", "OK", "WARN", "OK" };
    for (int r = 0; r < 4; r++) {
        LVITEMA it; memset(&it, 0, sizeof(it));
        it.mask = LVIF_TEXT; it.iItem = r; it.iSubItem = 0;
        it.pszText = (LPSTR)names[r];
        SendMessageA(hList, LVM_INSERTITEMA, 0, (LPARAM)&it);
        LVITEMA s1; memset(&s1, 0, sizeof(s1));
        s1.iSubItem = 1; s1.pszText = (LPSTR)codes[r];
        SendMessageA(hList, LVM_SETITEMTEXTA, r, (LPARAM)&s1);
        LVITEMA s2; memset(&s2, 0, sizeof(s2));
        s2.iSubItem = 2; s2.pszText = (LPSTR)states[r];
        SendMessageA(hList, LVM_SETITEMTEXTA, r, (LPARAM)&s2);
    }

    if ((int)SendMessageA(hList, LVM_GETITEMCOUNT, 0, 0) != 4) return 3;

    char buf[64];
    LVITEMA gi; memset(&gi, 0, sizeof(gi));
    gi.iSubItem = 0; gi.pszText = buf; gi.cchTextMax = sizeof(buf);
    SendMessageA(hList, LVM_GETITEMTEXTA, 1, (LPARAM)&gi);
    if (strcmp(buf, "Bravo") != 0) return 4;

    LVITEMA gs; memset(&gs, 0, sizeof(gs));
    gs.iSubItem = 1; gs.pszText = buf; gs.cchTextMax = sizeof(buf);
    SendMessageA(hList, LVM_GETITEMTEXTA, 2, (LPARAM)&gs);
    if (strcmp(buf, "C-23") != 0) return 5;

    LVITEMA st; memset(&st, 0, sizeof(st));
    st.state = LVIS_SELECTED | LVIS_FOCUSED; st.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    SendMessageA(hList, LVM_SETITEMSTATE, 2, (LPARAM)&st);
    if ((int)SendMessageA(hList, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED) != 2) return 6;

    /* ---- TreeView: a root, 3 children, 1 grandchild (5 nodes) ---- */
    TVINSERTSTRUCTA tv; memset(&tv, 0, sizeof(tv));
    tv.hParent = TVI_ROOT; tv.hInsertAfter = TVI_LAST;
    tv.item.mask = TVIF_TEXT; tv.item.pszText = (LPSTR)"Project";
    HTREEITEM hRoot = (HTREEITEM)SendMessageA(hTree, TVM_INSERTITEMA, 0, (LPARAM)&tv);

    HTREEITEM hKids[3];
    static const char *kid[3] = { "src", "include", "docs" };
    for (int i = 0; i < 3; i++) {
        TVINSERTSTRUCTA k; memset(&k, 0, sizeof(k));
        k.hParent = hRoot; k.hInsertAfter = TVI_LAST;
        k.item.mask = TVIF_TEXT; k.item.pszText = (LPSTR)kid[i];
        hKids[i] = (HTREEITEM)SendMessageA(hTree, TVM_INSERTITEMA, 0, (LPARAM)&k);
    }
    TVINSERTSTRUCTA gk; memset(&gk, 0, sizeof(gk));
    gk.hParent = hKids[0]; gk.hInsertAfter = TVI_LAST;
    gk.item.mask = TVIF_TEXT; gk.item.pszText = (LPSTR)"main.c";
    SendMessageA(hTree, TVM_INSERTITEMA, 0, (LPARAM)&gk);

    SendMessageA(hTree, TVM_EXPAND, TVE_EXPAND, (LPARAM)hRoot);

    if ((int)SendMessageA(hTree, TVM_GETCOUNT, 0, 0) != 5) return 7;
    if ((HTREEITEM)SendMessageA(hTree, TVM_GETNEXTITEM, TVGN_ROOT, 0) != hRoot) return 8;
    SendMessageA(hTree, TVM_SELECTITEM, TVGN_CARET, (LPARAM)hKids[1]);
    if ((HTREEITEM)SendMessageA(hTree, TVM_GETNEXTITEM, TVGN_CARET, 0) != hKids[1]) return 9;

    /* ---- Progress: 0..100, set 60, then StepIt by step 10 -> 70 ---- */
    SendMessageA(hProg, PBM_SETRANGE32, 0, 100);
    SendMessageA(hProg, PBM_SETSTEP, 10, 0);
    SendMessageA(hProg, PBM_SETPOS, 60, 0);
    if ((int)SendMessageA(hProg, PBM_GETPOS, 0, 0) != 60) return 10;
    SendMessageA(hProg, PBM_STEPIT, 0, 0);
    if ((int)SendMessageA(hProg, PBM_GETPOS, 0, 0) != 70) return 11;

    /* ---- StatusBar: 2 parts, read part 1 back ---- */
    {
        int edges[2] = { 200, -1 };
        SendMessageA(hStatus, SB_SETPARTS, 2, (LPARAM)edges);
        SendMessageA(hStatus, SB_SETTEXTA, 0, (LPARAM)"Ready");
        SendMessageA(hStatus, SB_SETTEXTA, 1, (LPARAM)"Items: 4");
        char sb[64];
        SendMessageA(hStatus, SB_GETTEXTA, 1, (LPARAM)sb);
        if (strcmp(sb, "Items: 4") != 0) return 12;
    }

    /* ---- Toolbar: add 3 buttons, read count back ---- */
    {
        TBBUTTON tb[3]; memset(tb, 0, sizeof(tb));
        for (int i = 0; i < 3; i++) { tb[i].iBitmap = i; tb[i].idCommand = 700 + i;
                                      tb[i].fsState = TBSTATE_ENABLED; tb[i].fsStyle = TBSTYLE_BUTTON; }
        SendMessageA(hTool, TB_ADDBUTTONS, 3, (LPARAM)tb);
        if ((int)SendMessageA(hTool, TB_BUTTONCOUNT, 0, 0) != 3) return 13;
    }

    (void)hi;
    return 25;
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT full = { 0, 0, 560, 420 };
        HBRUSH bb = CreateSolidBrush(RGB(48, 58, 78));
        FillRect(dc, &full, bb);
        DeleteObject(bb);
        EndPaint(h, &ps);
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

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES |
                 ICC_BAR_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icc);

    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hi;
    wc.lpszClassName = "tobyc25";
    RegisterClassA(&wc);

    hMain = CreateWindowExA(0, "tobyc25", "tobyOS C25 -- comctl32 common controls",
                            WS_OVERLAPPEDWINDOW, 80, 80, 560, 420, 0, 0, hi, 0);

    hTool = CreateWindowExA(0, TOOLBARCLASSNAMEA, "",
                            WS_CHILD | WS_VISIBLE, 0, 0, 0, 28,
                            hMain, (HMENU)(LONG_PTR)ID_TOOL, hi, 0);

    hList = CreateWindowExA(0, WC_LISTVIEWA, "",
                            WS_CHILD | WS_VISIBLE | LVS_REPORT,
                            14, 36, 300, 160,
                            hMain, (HMENU)(LONG_PTR)ID_LIST, hi, 0);

    hTree = CreateWindowExA(0, WC_TREEVIEWA, "",
                            WS_CHILD | WS_VISIBLE |
                            TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT,
                            326, 36, 210, 160,
                            hMain, (HMENU)(LONG_PTR)ID_TREE, hi, 0);

    hProg = CreateWindowExA(0, PROGRESS_CLASSA, "",
                            WS_CHILD | WS_VISIBLE,
                            14, 210, 522, 22,
                            hMain, (HMENU)(LONG_PTR)ID_PROG, hi, 0);

    hStatus = CreateWindowExA(0, STATUSCLASSNAMEA, "",
                              WS_CHILD | WS_VISIBLE | CCS_BOTTOM, 0, 0, 0, 0,
                              hMain, (HMENU)(LONG_PTR)ID_STAT, hi, 0);

    if (!hMain || !hTool || !hList || !hTree || !hProg || !hStatus)
        return 2;

    g_rc = build_and_check(hi);

    ShowWindow(hMain, sh);
    UpdateWindow(hMain);

    MSG msg;
    int slept = 0;
    /* Pump messages so the controls render (for a screenshot), then self-close. */
    while (GetMessageA(&msg, 0, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
        if (!g_done) { g_done = 1; }
        if (g_done && !slept) { Sleep(1500); slept = 1; DestroyWindow(hMain); }
    }
    return g_rc;
}
