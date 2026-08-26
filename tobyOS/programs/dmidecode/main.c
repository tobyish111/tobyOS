/* dmidecode -- decode the firmware's SMBIOS/DMI table.
 *
 * Reads exactly what the real dmidecode(8) reads:
 *   /sys/firmware/dmi/tables/smbios_entry_point   (the anchor)
 *   /sys/firmware/dmi/tables/DMI                  (the structure table)
 * Both are binary, both are 0400, and both are published by src/smbios.c
 * from whatever the firmware handed the bootloader.
 *
 * Decoding lives here rather than in the kernel on purpose: the kernel's
 * job is to publish the bytes at the standard path, and once it does,
 * ANY tool that knows the format works -- this one is the demonstration.
 *
 * What it prints for a field it cannot resolve matters. The SMBIOS spec
 * has three distinct "no value" cases and they are not interchangeable:
 *   - string index 0        -> "Not Specified" (firmware said nothing)
 *   - a documented sentinel -> "Unknown"       (firmware said "I don't know")
 *   - an enum we lack a name for -> the raw number, never a guess.
 *
 *   -t TYPE     only structures of this type (repeatable)
 *   -s KEYWORD  print one value, unadorned (scripting form)
 *   -q          quiet: skip handles/headers
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#define ENTRY_PATH "/sys/firmware/dmi/tables/smbios_entry_point"
#define TABLE_PATH "/sys/firmware/dmi/tables/DMI"
#define MAX_TABLE  (256 * 1024)

static unsigned char g_entry[64];
static size_t        g_entry_len;
static unsigned char g_table[MAX_TABLE];
static size_t        g_table_len;

static int   g_quiet;
static int   g_want[256];
static int   g_want_any;

static unsigned rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned rd32(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

/* `why` receives the errno that stopped us, or 0 if the file opened and
 * simply held nothing. Distinguishing those two matters: the raw DMI
 * tables are mode 0400 (Linux publishes them that way, and so do we), so
 * the overwhelmingly common failure is "you are not root" -- and saying
 * "no SMBIOS table published by this firmware" to a user who merely
 * forgot sudo is a claim about their MACHINE that we have not
 * established. */
static size_t slurp(const char *path, unsigned char *buf, size_t cap, int *why) {
    if (why) *why = 0;
    FILE *f = fopen(path, "rb");
    if (!f) { if (why) *why = errno ? errno : EACCES; return 0; }
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

/* Total on-wire length of a structure: the formatted area plus the
 * string set, which ends at a double NUL. */
static size_t struct_len(const unsigned char *s, size_t avail) {
    if (avail < 4) return 0;
    size_t flen = s[1];
    if (flen < 4 || flen > avail) return 0;
    size_t i = flen;
    while (i + 1 < avail && !(s[i] == 0 && s[i + 1] == 0)) i++;
    if (i + 1 >= avail) return avail;
    return i + 2;
}

/* 1-based index into the structure's string set. */
static const char *dmi_string(const unsigned char *s, size_t slen, unsigned idx) {
    if (idx == 0) return NULL;
    size_t flen = s[1];
    const char *p = (const char *)(s + flen);
    const char *end = (const char *)(s + slen);
    for (unsigned i = 1; p < end; i++) {
        size_t l = 0;
        while (p + l < end && p[l]) l++;
        if (l == 0) return NULL;
        if (i == idx) return p;
        p += l + 1;
    }
    return NULL;
}

static void put_str(const char *label, const unsigned char *s, size_t slen,
                    unsigned idx) {
    const char *v = dmi_string(s, slen, idx);
    printf("\t%s: %s\n", label, v ? v : "Not Specified");
}

/* SMBIOS 7.4.1 chassis types. */
static const char *chassis_type(unsigned t) {
    static const char *n[] = {
        NULL, "Other", "Unknown", "Desktop", "Low Profile Desktop",
        "Pizza Box", "Mini Tower", "Tower", "Portable", "Laptop",
        "Notebook", "Hand Held", "Docking Station", "All In One",
        "Sub Notebook", "Space-saving", "Lunch Box", "Main Server Chassis",
        "Expansion Chassis", "Sub Chassis", "Bus Expansion Chassis",
        "Peripheral Chassis", "RAID Chassis", "Rack Mount Chassis",
        "Sealed-case PC", "Multi-system", "CompactPCI", "AdvancedTCA",
        "Blade", "Blade Enclosing", "Tablet", "Convertible", "Detachable",
        "IoT Gateway", "Embedded PC", "Mini PC", "Stick PC"
    };
    t &= 0x7f;
    if (t < sizeof(n) / sizeof(n[0]) && n[t]) return n[t];
    return NULL;
}

/* SMBIOS 7.18.2 memory device types -- the subset that exists in the wild. */
static const char *memory_type(unsigned t) {
    static const char *n[] = {
        NULL, "Other", "Unknown", "DRAM", "EDRAM", "VRAM", "SRAM", "RAM",
        "ROM", "Flash", "EEPROM", "FEPROM", "EPROM", "CDRAM", "3DRAM",
        "SDRAM", "SGRAM", "RDRAM", "DDR", "DDR2", "DDR2 FB-DIMM",
        NULL, NULL, NULL, "DDR3", "FBD2", "DDR4", "LPDDR", "LPDDR2",
        "LPDDR3", "LPDDR4", "Logical non-volatile device", "HBM", "HBM2",
        "DDR5", "LPDDR5"
    };
    if (t < sizeof(n) / sizeof(n[0]) && n[t]) return n[t];
    return NULL;
}

static const char *form_factor(unsigned t) {
    static const char *n[] = {
        NULL, "Other", "Unknown", "SIMM", "SIP", "Chip", "DIP", "ZIP",
        "Proprietary Card", "DIMM", "TSOP", "Row of chips", "RIMM",
        "SODIMM", "SRIMM", "FB-DIMM", "Die"
    };
    if (t < sizeof(n) / sizeof(n[0]) && n[t]) return n[t];
    return NULL;
}

static const char *processor_type(unsigned t) {
    switch (t) {
        case 1: return "Other";
        case 2: return "Unknown";
        case 3: return "Central Processor";
        case 4: return "Math Processor";
        case 5: return "DSP Processor";
        case 6: return "Video Processor";
        default: return NULL;
    }
}

static const char *type_name(unsigned t) {
    switch (t) {
        case 0:  return "BIOS Information";
        case 1:  return "System Information";
        case 2:  return "Base Board Information";
        case 3:  return "Chassis Information";
        case 4:  return "Processor Information";
        case 7:  return "Cache Information";
        case 8:  return "Port Connector Information";
        case 9:  return "System Slot Information";
        case 11: return "OEM Strings";
        case 16: return "Physical Memory Array";
        case 17: return "Memory Device";
        case 19: return "Memory Array Mapped Address";
        case 32: return "System Boot Information";
        case 127: return "End Of Table";
        default: return NULL;
    }
}

/* An enum value we have no name for prints as the raw number. Inventing a
 * label for an unknown code would be exactly the kind of plausible lie
 * this whole tree exists to avoid. */
static void put_enum(const char *label, const char *name, unsigned raw) {
    if (name) printf("\t%s: %s\n", label, name);
    else      printf("\t%s: %u\n", label, raw);
}

static void decode(const unsigned char *s, size_t slen) {
    unsigned type = s[0];
    unsigned flen = s[1];

    switch (type) {
        case 0:
            put_str("Vendor",       s, slen, s[4]);
            put_str("Version",      s, slen, s[5]);
            put_str("Release Date", s, slen, s[8]);
            if (flen > 9 && s[9] != 0xff)
                printf("\tROM Size: %u kB\n", (s[9] + 1u) * 64u);
            break;

        case 1:
            put_str("Manufacturer", s, slen, s[4]);
            put_str("Product Name", s, slen, s[5]);
            put_str("Version",      s, slen, s[6]);
            put_str("Serial Number", s, slen, s[7]);
            if (flen >= 0x19) {
                const unsigned char *u = s + 8;
                int all_ff = 1, all_00 = 1;
                for (int i = 0; i < 16; i++) {
                    if (u[i] != 0xff) all_ff = 0;
                    if (u[i] != 0x00) all_00 = 0;
                }
                if (all_ff)      printf("\tUUID: Not Present\n");
                else if (all_00) printf("\tUUID: Not Settable\n");
                else {
                    /* SMBIOS >= 2.6 stores the first three fields
                     * little-endian; earlier versions big-endian. */
                    printf("\tUUID: %02x%02x%02x%02x-%02x%02x-%02x%02x-"
                           "%02x%02x-%02x%02x%02x%02x%02x%02x\n",
                           u[3], u[2], u[1], u[0], u[5], u[4], u[7], u[6],
                           u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
                }
            }
            if (flen >= 0x1b) {
                put_str("SKU Number", s, slen, s[0x19]);
                put_str("Family",     s, slen, s[0x1a]);
            }
            break;

        case 2:
            put_str("Manufacturer",  s, slen, s[4]);
            put_str("Product Name",  s, slen, s[5]);
            put_str("Version",       s, slen, s[6]);
            put_str("Serial Number", s, slen, s[7]);
            if (flen > 8) put_str("Asset Tag", s, slen, s[8]);
            break;

        case 3:
            put_str("Manufacturer", s, slen, s[4]);
            if (flen > 5) put_enum("Type", chassis_type(s[5]), s[5] & 0x7f);
            if (flen > 6) put_str("Version",       s, slen, s[6]);
            if (flen > 7) put_str("Serial Number", s, slen, s[7]);
            if (flen > 8) put_str("Asset Tag",     s, slen, s[8]);
            break;

        case 4:
            put_str("Socket Designation", s, slen, s[4]);
            if (flen > 5) put_enum("Type", processor_type(s[5]), s[5]);
            put_str("Manufacturer", s, slen, s[7]);
            if (flen > 0x10) put_str("Version", s, slen, s[0x10]);
            if (flen > 0x13 && rd16(s + 0x12))
                printf("\tExternal Clock: %u MHz\n", rd16(s + 0x12));
            if (flen > 0x15 && rd16(s + 0x14))
                printf("\tMax Speed: %u MHz\n", rd16(s + 0x14));
            if (flen > 0x17) {
                unsigned cur = rd16(s + 0x16);
                if (cur) printf("\tCurrent Speed: %u MHz\n", cur);
                else     printf("\tCurrent Speed: Unknown\n");
            }
            if (flen > 0x25) {
                if (s[0x23]) printf("\tCore Count: %u\n", s[0x23]);
                if (s[0x24]) printf("\tCore Enabled: %u\n", s[0x24]);
                if (s[0x25]) printf("\tThread Count: %u\n", s[0x25]);
            }
            break;

        case 17: {
            if (flen > 0x0d) {
                unsigned sz = rd16(s + 0x0c);
                if (sz == 0)           printf("\tSize: No Module Installed\n");
                else if (sz == 0xffff) printf("\tSize: Unknown\n");
                else if (sz == 0x7fff && flen > 0x1f)
                    printf("\tSize: %u MB\n", rd32(s + 0x1c) & 0x7fffffffu);
                else if (sz & 0x8000)  printf("\tSize: %u kB\n", sz & 0x7fff);
                else                   printf("\tSize: %u MB\n", sz);
            }
            if (flen > 0x0e) put_enum("Form Factor", form_factor(s[0x0e]), s[0x0e]);
            if (flen > 0x10) put_str("Locator",      s, slen, s[0x10]);
            if (flen > 0x11) put_str("Bank Locator", s, slen, s[0x11]);
            if (flen > 0x12) put_enum("Type", memory_type(s[0x12]), s[0x12]);
            if (flen > 0x16) {
                unsigned sp = rd16(s + 0x15);
                if (sp) printf("\tSpeed: %u MT/s\n", sp);
                else    printf("\tSpeed: Unknown\n");
            }
            if (flen > 0x17) put_str("Manufacturer",  s, slen, s[0x17]);
            if (flen > 0x18) put_str("Serial Number", s, slen, s[0x18]);
            if (flen > 0x1a) put_str("Part Number",   s, slen, s[0x1a]);
            break;
        }

        case 127:
            break;

        default:
            /* Every other type is listed but not interpreted. Printing the
             * header alone is honest; inventing field names is not. */
            break;
    }
}

/* dmidecode -s takes a keyword naming exactly one field. */
struct kw { const char *name; unsigned type; unsigned off; };
static const struct kw g_keywords[] = {
    { "bios-vendor",           0, 4 },
    { "bios-version",          0, 5 },
    { "bios-release-date",     0, 8 },
    { "system-manufacturer",   1, 4 },
    { "system-product-name",   1, 5 },
    { "system-version",        1, 6 },
    { "system-serial-number",  1, 7 },
    { "baseboard-manufacturer",2, 4 },
    { "baseboard-product-name",2, 5 },
    { "baseboard-version",     2, 6 },
    { "baseboard-serial-number",2, 7 },
    { "chassis-manufacturer",  3, 4 },
    { "chassis-version",       3, 6 },
    { "chassis-serial-number", 3, 7 },
    { "processor-manufacturer",4, 7 },
    { "processor-version",     4, 0x10 },
};

static int do_keyword(const char *want) {
    const struct kw *k = NULL;
    for (size_t i = 0; i < sizeof(g_keywords) / sizeof(g_keywords[0]); i++) {
        if (strcmp(g_keywords[i].name, want) == 0) { k = &g_keywords[i]; break; }
    }
    if (!k) {
        fprintf(stderr, "dmidecode: unknown keyword '%s'; valid are:\n", want);
        for (size_t i = 0; i < sizeof(g_keywords) / sizeof(g_keywords[0]); i++)
            fprintf(stderr, "  %s\n", g_keywords[i].name);
        return 2;
    }
    size_t off = 0;
    while (off + 4 <= g_table_len) {
        const unsigned char *s = g_table + off;
        size_t slen = struct_len(s, g_table_len - off);
        if (!slen) break;
        if (s[0] == k->type && s[1] > k->off) {
            const char *v = dmi_string(s, slen, s[k->off]);
            if (v) { printf("%s\n", v); return 0; }
        }
        if (s[0] == 127) break;
        off += slen;
    }
    /* Real dmidecode is silent and exits 1 when the field is absent. */
    return 1;
}

int main(int argc, char **argv) {
    const char *keyword = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-q") == 0 || strcmp(a, "--quiet") == 0) { g_quiet = 1; }
        else if (strcmp(a, "-t") == 0 || strcmp(a, "--type") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "dmidecode: -t needs a type\n"); return 2; }
            int t = atoi(argv[++i]);
            if (t < 0 || t > 255) { fprintf(stderr, "dmidecode: bad type\n"); return 2; }
            g_want[t] = 1; g_want_any = 1;
        }
        else if (strcmp(a, "-s") == 0 || strcmp(a, "--string") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "dmidecode: -s needs a keyword\n"); return 2; }
            keyword = argv[++i];
        }
        else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            printf("usage: dmidecode [-q] [-t TYPE] [-s KEYWORD]\n");
            return 0;
        }
        else { fprintf(stderr, "dmidecode: unknown option '%s'\n", a); return 2; }
    }

    int ewhy = 0, twhy = 0;
    g_entry_len = slurp(ENTRY_PATH, g_entry, sizeof g_entry, &ewhy);
    g_table_len = slurp(TABLE_PATH, g_table, sizeof g_table, &twhy);
    (void)ewhy;
    if (!g_table_len) {
        if (twhy == EACCES || twhy == EPERM)
            fprintf(stderr, "dmidecode: cannot read %s: permission denied "
                            "-- the raw DMI tables are mode 0400, so this "
                            "needs to run as root\n", TABLE_PATH);
        else if (twhy == ENOENT)
            fprintf(stderr, "dmidecode: %s does not exist: no SMBIOS table "
                            "was published by this firmware\n", TABLE_PATH);
        else
            fprintf(stderr, "dmidecode: cannot read %s: %s\n",
                    TABLE_PATH, strerror(twhy));
        return 1;
    }

    if (keyword) return do_keyword(keyword);

    if (!g_quiet) {
        printf("# dmidecode 3.4 (tobyOS native)\n");
        printf("Getting SMBIOS data from sysfs.\n");
        if (g_entry_len >= 5 && memcmp(g_entry, "_SM3_", 5) == 0) {
            printf("SMBIOS %u.%u.%u present.\n",
                   g_entry[7], g_entry[8], g_entry[9]);
        } else if (g_entry_len >= 4 && memcmp(g_entry, "_SM_", 4) == 0) {
            printf("SMBIOS %u.%u present.\n", g_entry[6], g_entry[7]);
            printf("%u structures occupying %u bytes.\n",
                   rd16(g_entry + 0x1c), rd16(g_entry + 0x16));
        }
        printf("\n");
    }

    size_t off = 0;
    int shown = 0;
    while (off + 4 <= g_table_len) {
        const unsigned char *s = g_table + off;
        size_t slen = struct_len(s, g_table_len - off);
        if (!slen) break;
        unsigned type = s[0];

        if (!g_want_any || g_want[type]) {
            const char *tn = type_name(type);
            if (!g_quiet) {
                printf("Handle 0x%04X, DMI type %u, %u bytes\n",
                       rd16(s + 2), type, s[1]);
            }
            /* An unnamed type still gets its number -- "DMI type 42" is a
             * fact; a made-up name would not be. */
            if (tn) printf("%s\n", tn);
            else    printf("OEM-specific Type %u\n", type);
            decode(s, slen);
            printf("\n");
            shown++;
        }
        if (type == 127) break;
        off += slen;
    }

    if (g_want_any && shown == 0) {
        fprintf(stderr, "dmidecode: no structures of the requested type\n");
        return 1;
    }
    return 0;
}
