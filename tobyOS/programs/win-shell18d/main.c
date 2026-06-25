/* C18d first-party test: shell32 (ShellExecuteA / SHGetFolderPathA /
 * SHGetSpecialFolderPathA / Shell_NotifyIconA / DragQueryFileA).
 *
 * Rigorous, kernel-verifiable proof (like C18a's DB readback): the test asks
 * SHGetFolderPathA(CSIDL_APPDATA | CSIDL_FLAG_CREATE) for the roaming-appdata
 * path, then CreateFile + WriteFile a sentinel there. That path is a pure
 * shell32 construct (C:\Users\toby\AppData\Roaming) -- if the kernel can read
 * "C18D-SHELL-OK" back from /data/Users/toby/AppData/Roaming/c18d.txt, the CSIDL
 * mapping genuinely resolved end-to-end through the C:\ -> /data file layer and
 * CSIDL_FLAG_CREATE built the tree. The remaining shims are proven by their
 * return values (encoded in exit 88). */
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>

static int is_cdrive(const char *s) { return s[0] == 'C' && s[1] == ':' && s[2] == '\\'; }

int main(void) {
    char appdata[MAX_PATH] = {0};
    char docs[MAX_PATH]    = {0};

    HRESULT hr = SHGetFolderPathA(NULL, CSIDL_APPDATA | CSIDL_FLAG_CREATE, NULL, 0, appdata);
    BOOL    gp = SHGetSpecialFolderPathA(NULL, docs, CSIDL_PERSONAL, TRUE);

    /* Write a sentinel under the returned APPDATA path (the kernel re-reads it). */
    int wrote = 0;
    if (hr == 0 && is_cdrive(appdata)) {
        char fpath[MAX_PATH];
        int n = 0; for (; appdata[n]; n++) fpath[n] = appdata[n];
        const char *suf = "\\c18d.txt";
        for (int k = 0; suf[k]; k++) fpath[n++] = suf[k];
        fpath[n] = 0;
        HANDLE h = CreateFileA(fpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD wr = 0; const char *msg = "C18D-SHELL-OK";
            WriteFile(h, msg, 13, &wr, NULL);
            CloseHandle(h);
            wrote = (wr == 13);
        }
    }

    HINSTANCE se = ShellExecuteA(NULL, "open", "C:\\Users\\toby\\Documents",
                                 NULL, NULL, SW_SHOWNORMAL);

    NOTIFYICONDATAA nid; memset(&nid, 0, sizeof nid);
    nid.cbSize = sizeof nid; nid.uID = 1;
    BOOL tray = Shell_NotifyIconA(NIM_ADD, &nid);

    UINT ndrag = DragQueryFileA((HDROP)0, 0xFFFFFFFFu, NULL, 0);

    int ok = (hr == 0) && is_cdrive(appdata)
          && gp && is_cdrive(docs)
          && wrote
          && ((INT_PTR)se > 32)
          && tray
          && (ndrag == 0);

    printf("[shell18d] appdata=%s docs=%s wrote=%d se=%lld tray=%d ndrag=%u ok=%d\n",
           appdata, docs, wrote, (long long)(INT_PTR)se, (int)tray, ndrag, ok);
    fflush(stdout);
    return ok ? 88 : 1;
}
