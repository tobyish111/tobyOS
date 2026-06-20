/* main.c -- a STOCK Win32 GUI .exe whose entire UI is a MODAL dialog built from
 * a RESOURCE template (Track C / C14a). WinMain calls DialogBoxParamA with a
 * MAKEINTRESOURCE id; tobyOS walks the PE's .rsrc section to find the RT_DIALOG
 * resource, parses the DLGTEMPLATEEX + its DLGITEMTEMPLATEEX controls, builds
 * the dialog window + controls, runs a modal loop, and dispatches to DlgProc.
 *
 * The dialog has an EDIT, an AUTOCHECKBOX, two AUTORADIOBUTTONs in a GROUPBOX,
 * and OK/Cancel. On OK the DlgProc reads the controls back and EndDialog()s with
 * a code that becomes the process exit code:
 *   14  the EDIT held "tobyOS", the checkbox was checked, and "Premium" chosen
 *    3  EDIT text wrong
 *    4  checkbox not checked
 *    5  neither/ wrong radio
 *    0  Cancel
 */

#include <windows.h>

#define IDD_MAIN   101
#define IDC_EDIT   1001
#define IDC_CHECK  1002
#define IDC_RAD_A  1003
#define IDC_RAD_B  1004

static INT_PTR CALLBACK DlgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemTextA(h, IDC_EDIT, "edit me");
        CheckRadioButton(h, IDC_RAD_A, IDC_RAD_B, IDC_RAD_A);  /* default Free */
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: {
            char buf[64];
            GetDlgItemTextA(h, IDC_EDIT, buf, sizeof(buf));
            int checked = (IsDlgButtonChecked(h, IDC_CHECK) == BST_CHECKED);
            int premium = (IsDlgButtonChecked(h, IDC_RAD_B) == BST_CHECKED);
            int code;
            if (lstrcmpA(buf, "tobyOS") != 0) code = 3;
            else if (!checked)               code = 4;
            else if (!premium)               code = 5;
            else                             code = 14;
            EndDialog(h, code);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(h, 0);
            return TRUE;
        }
        return FALSE;
    }
    return FALSE;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show) {
    (void)hPrev; (void)cmd; (void)show;
    INT_PTR r = DialogBoxParamA(hInst, MAKEINTRESOURCEA(IDD_MAIN), NULL, DlgProc, 0);
    return (int)r;
}
