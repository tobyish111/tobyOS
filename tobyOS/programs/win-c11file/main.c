/* main.c -- a STOCK Windows console .exe exercising the EXTENDED Win32 file API
 * (Track C / C11): CreateDirectory / GetFileAttributes / GetFileSize /
 * SetFilePointer / FindFirstFile+FindNextFile+FindClose / DeleteFile, on top of
 * the C5 CreateFile/Read/Write/CloseHandle base. tobyOS maps these onto the VFS
 * (a "C:" drive -> the writable /data mount).
 *
 * Exit code encodes the proof (distinct codes per step):
 *   2  CreateDirectory / GetFileAttributes(dir) failed
 *   3  CreateFile failed
 *   4  GetFileSize wasn't 16
 *   5  SetFilePointer(10)+ReadFile didn't return "ABCDEF"
 *   6  FindFirstFile/FindNextFile didn't enumerate the file we created
 *   7  DeleteFile failed
 *   8  the file still exists after DeleteFile
 *  11  ALL PASSED
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    CreateDirectoryA("C:\\c11dir", NULL);
    DWORD da = GetFileAttributesA("C:\\c11dir");
    if (da == INVALID_FILE_ATTRIBUTES || !(da & FILE_ATTRIBUTE_DIRECTORY))
        return 2;

    HANDLE h = CreateFileA("C:\\c11dir\\f.txt", GENERIC_READ | GENERIC_WRITE,
                           0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 3;

    const char *msg = "0123456789ABCDEF";
    DWORD wr = 0;
    WriteFile(h, msg, 16, &wr, NULL);
    if (GetFileSize(h, NULL) != 16) { CloseHandle(h); return 4; }

    /* seek to offset 10, read 6 -> "ABCDEF" */
    SetFilePointer(h, 10, NULL, FILE_BEGIN);
    char buf[7] = {0};
    DWORD rd = 0;
    ReadFile(h, buf, 6, &rd, NULL);
    CloseHandle(h);
    if (rd != 6 || memcmp(buf, "ABCDEF", 6) != 0) return 5;

    /* enumerate the directory and find our file */
    WIN32_FIND_DATAA fdata;
    HANDLE fh = FindFirstFileA("C:\\c11dir\\*", &fdata);
    int found = 0;
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fdata.cFileName, "f.txt") == 0) found = 1;
        } while (FindNextFileA(fh, &fdata));
        FindClose(fh);
    }
    if (!found) return 6;

    if (!DeleteFileA("C:\\c11dir\\f.txt")) return 7;
    if (GetFileAttributesA("C:\\c11dir\\f.txt") != INVALID_FILE_ATTRIBUTES)
        return 8;   /* must be gone now */

    printf("C11 file ops: dir + create + write + size(16) + seek/read(ABCDEF) + "
           "find(f.txt) + delete -> OK\n");
    return 11;
}
