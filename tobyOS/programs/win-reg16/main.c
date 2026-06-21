/* main.c -- a STOCK Win32 console .exe exercising the registry (Track C / C16a).
 * It reads a persisted DWORD counter under HKCU\Software\tobyOS\C16, increments
 * it, writes it back (+ a REG_SZ "Magic" value), and verifies the round-trip.
 * tobyOS backs the Reg* API with an in-kernel hive flushed to /data, so the
 * counter survives a reboot.
 *
 * Exit code:
 *   2  RegCreateKeyEx failed
 *   3  RegOpenKeyEx (re-open) failed
 *   4  the DWORD round-trip didn't match
 *   5  the REG_SZ "Magic" value didn't read back
 *  17  round-trip OK, FIRST write (no prior persisted counter)
 *  16  round-trip OK AND a prior counter persisted (>=1) -- persistence proven
 */

#include <windows.h>

int main(void) {
    HKEY hk;
    DWORD count = 0, sz = sizeof(count), type = 0;

    /* Read any previously-persisted counter. */
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\tobyOS\\C16", 0, KEY_READ, &hk) == ERROR_SUCCESS) {
        RegQueryValueExA(hk, "Count", 0, &type, (BYTE *)&count, &sz);
        RegCloseKey(hk);
    }

    /* Increment + write back. */
    DWORD next = count + 1;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\tobyOS\\C16", 0, NULL, 0,
                        KEY_WRITE, NULL, &hk, NULL) != ERROR_SUCCESS)
        return 2;
    RegSetValueExA(hk, "Count", 0, REG_DWORD, (const BYTE *)&next, sizeof(next));
    RegSetValueExA(hk, "Magic", 0, REG_SZ, (const BYTE *)"tobyOS-C16a", 12);
    RegCloseKey(hk);

    /* Verify the round-trip (re-open + read back). */
    DWORD check = 0; sz = sizeof(check); type = 0;
    char magic[32]; DWORD msz = sizeof(magic);
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\tobyOS\\C16", 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return 3;
    RegQueryValueExA(hk, "Count", 0, &type, (BYTE *)&check, &sz);
    RegQueryValueExA(hk, "Magic", 0, NULL, (BYTE *)magic, &msz);
    RegCloseKey(hk);

    if (check != next) return 4;
    if (lstrcmpA(magic, "tobyOS-C16a") != 0) return 5;
    return (count >= 1) ? 16 : 17;
}
