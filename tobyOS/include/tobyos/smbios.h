/* smbios.h -- SMBIOS / DMI table discovery (2026-08-24).
 *
 * The firmware describes the physical machine -- board, BIOS, chassis,
 * CPU sockets, DIMMs -- in a table whose entry point lives either in the
 * BIOS F-segment or in a UEFI configuration table. tobyOS had no access
 * to any of it, which is why `dmidecode` had no data source.
 *
 * This module only FINDS and VALIDATES the table. It does not interpret
 * it beyond the handful of strings /sys/class/dmi/id has to publish;
 * decoding structures is dmidecode(8)'s job, reading the bytes we
 * republish at the standard Linux path.
 */

#ifndef TOBYOS_SMBIOS_H
#define TOBYOS_SMBIOS_H

#include <tobyos/types.h>

struct smbios_info {
    bool           present;
    const uint8_t *entry;       /* kernel-virtual entry point            */
    size_t         entry_len;   /* 0x1F for _SM_, 0x18 for _SM3_         */
    const uint8_t *table;       /* kernel-virtual structure table        */
    size_t         table_len;
    uint16_t       count;       /* 0 when unknown (SMBIOS 3.x omits it)  */
    uint8_t        major, minor;
    bool           is_64;       /* found via the 3.0 "_SM3_" entry point */
    const char    *source;      /* "limine-64" / "limine-32" / "f-scan"  */
};

/* Locate the table. `entry32`/`entry64` are Limine's response pointers
 * (either may be NULL); when both are unusable we fall back to scanning
 * the BIOS F-segment. Safe to call with no SMBIOS present -- the info
 * block simply stays `present == false`, and every consumer must then
 * publish NOTHING rather than a plausible default. */
void smbios_init(void *entry32, void *entry64);

const struct smbios_info *smbios_get(void);

/* One DMI id string, or NULL if the machine did not report it. `which`
 * is a SMBIOS_ID_* selector. Returns a pointer into a kernel-owned
 * buffer that is stable for the life of the system. */
const char *smbios_id_string(int which);

enum {
    SMBIOS_ID_BIOS_VENDOR = 0,
    SMBIOS_ID_BIOS_VERSION,
    SMBIOS_ID_BIOS_DATE,
    SMBIOS_ID_SYS_VENDOR,
    SMBIOS_ID_PRODUCT_NAME,
    SMBIOS_ID_PRODUCT_VERSION,
    SMBIOS_ID_PRODUCT_SERIAL,
    SMBIOS_ID_BOARD_VENDOR,
    SMBIOS_ID_BOARD_NAME,
    SMBIOS_ID_BOARD_VERSION,
    SMBIOS_ID_BOARD_SERIAL,
    SMBIOS_ID_CHASSIS_VENDOR,
    SMBIOS_ID_MAX
};

#endif /* TOBYOS_SMBIOS_H */
