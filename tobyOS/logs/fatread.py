#!/usr/bin/env python3
"""fatread.py -- read a FAT32 image WITHOUT using any tobyOS code.

The point of this file is independence. src/fat32.c mounting a volume that
src/fat32.c formatted proves the two halves agree with each other and
nothing else; if both share a misreading of the spec, the test passes and
the stick still does not work in another machine.

So this is written from Microsoft's FAT specification directly -- offsets
and field meanings taken from the spec, not from the driver -- and it
checks the structural invariants a real FAT32 implementation relies on,
then reads back the files tobyOS wrote and compares the bytes.

    python logs/fatread.py <image>

Exit status is the result: 0 = the volume is a valid, readable FAT32.
"""
import struct, sys

fails = []
oks = []

def chk(cond, what, detail=""):
    (oks if cond else fails).append((what, detail))
    print(("  ok   " if cond else "  FAIL ") + what + ("   " + detail if detail else ""))

def u16(b, o): return struct.unpack_from("<H", b, o)[0]
def u32(b, o): return struct.unpack_from("<I", b, o)[0]

def main():
    path = sys.argv[1]
    img = open(path, "rb").read()
    print("== independent FAT32 read of " + path + " (" + str(len(img)) + " bytes)")

    # ---- boot sector, per the spec's BPB layout --------------------------
    bps      = u16(img, 11)
    spc      = img[13]
    rsvd     = u16(img, 14)
    nfats    = img[16]
    rootent  = u16(img, 17)
    totsec16 = u16(img, 19)
    media    = img[21]
    fatsz16  = u16(img, 22)
    totsec32 = u32(img, 32)
    fatsz32  = u32(img, 36)
    rootclus = u32(img, 44)
    fsinfo   = u16(img, 48)
    bkboot   = u16(img, 50)

    chk(img[510] == 0x55 and img[511] == 0xAA, "boot signature 0xAA55",
        "got %02x%02x" % (img[510], img[511]))
    chk(bps == 512, "bytes/sector is 512", "got %d" % bps)
    chk(spc in (1,2,4,8,16,32,64,128), "sectors/cluster is a power of two",
        "got %d" % spc)
    # These three are what DISTINGUISH FAT32 from FAT12/16 on disk.
    chk(rootent == 0, "root entry count is 0 (required for FAT32)", "got %d" % rootent)
    chk(fatsz16 == 0, "FATSz16 is 0 (required for FAT32)", "got %d" % fatsz16)
    chk(totsec16 == 0, "TotSec16 is 0, count lives in TotSec32", "got %d" % totsec16)
    chk(media in (0xF8, 0xF0), "media descriptor is legal", "got 0x%02x" % media)
    chk(fatsz32 > 0, "FATSz32 is set", "got %d" % fatsz32)
    chk(nfats in (1,2), "1 or 2 FATs", "got %d" % nfats)
    chk(totsec32 * bps <= len(img), "TotSec32 fits inside the image",
        "%d sectors x %d <= %d" % (totsec32, bps, len(img)))

    data_start = rsvd + nfats * fatsz32
    clusters = (totsec32 - data_start) // spc
    # The spec DEFINES the type by cluster count. <= 65524 is FAT16, and a
    # volume that claims FAT32 below that bar is read as FAT16 elsewhere.
    chk(clusters > 65524, "cluster count > 65524, so this really is FAT32",
        "got %d" % clusters)
    need = ((clusters + 2) * 4 + bps - 1) // bps
    chk(fatsz32 >= need, "FAT is large enough for every cluster",
        "FATSz32=%d need>=%d" % (fatsz32, need))

    # ---- FSInfo ----------------------------------------------------------
    if fsinfo:
        fo = fsinfo * bps
        chk(u32(img, fo) == 0x41615252, "FSInfo lead signature")
        chk(u32(img, fo + 484) == 0x61417272, "FSInfo struct signature")
        chk(u32(img, fo + 508) == 0xAA550000, "FSInfo trail signature")
    if bkboot:
        bo = bkboot * bps
        chk(img[bo:bo+90] == img[0:90], "backup boot sector matches the primary")

    # ---- the FAT ---------------------------------------------------------
    fat_off = rsvd * bps
    fat = img[fat_off: fat_off + fatsz32 * bps]
    e0 = u32(fat, 0) & 0x0FFFFFFF
    e1 = u32(fat, 4) & 0x0FFFFFFF
    chk((e0 & 0xFF) == media, "FAT[0] low byte is the media descriptor",
        "got 0x%08x" % e0)
    chk(e1 >= 0x0FFFFFF8, "FAT[1] is an end-of-chain marker", "got 0x%08x" % e1)
    if nfats == 2:
        f2 = img[fat_off + fatsz32*bps: fat_off + 2*fatsz32*bps]
        chk(fat == f2, "the two FATs are identical")

    # ---- walking chains and directories ----------------------------------
    def chain(start):
        out, c, guard = [], start, 0
        while 2 <= c < 0x0FFFFFF8 and guard < (1 << 20):
            out.append(c)
            c = u32(fat, c * 4) & 0x0FFFFFFF
            guard += 1
        return out, c

    def cluster_bytes(c):
        off = (data_start + (c - 2) * spc) * bps
        return img[off: off + spc * bps]

    def read_dir(start):
        ents = []
        clus, _term = chain(start)
        raw = b"".join(cluster_bytes(c) for c in clus)
        for o in range(0, len(raw), 32):
            e = raw[o:o+32]
            if len(e) < 32 or e[0] == 0x00: break     # 0 ends the directory
            if e[0] == 0xE5: continue                 # a deleted slot
            if (e[11] & 0x0F) == 0x0F: continue       # a long-name entry
            nm = e[0:11].decode("latin-1")
            base, ext = nm[0:8].rstrip(), nm[8:11].rstrip()
            ents.append({"raw": nm,
                         "name": base + ("." + ext if ext else ""),
                         "attr": e[11],
                         "clus": (u16(e, 20) << 16) | u16(e, 26),
                         "size": u32(e, 28),
                         "wrt_date": u16(e, 24)})
        return ents

    root = read_dir(rootclus)
    print("  root directory: " + ", ".join(e["name"] for e in root))

    # ---- the content tobyOS claims to have written ------------------------
    want = b"tobyOS wrote this to a FAT32 volume it formatted itself.\n"
    hello = [e for e in root if e["name"].upper() == "HELLO.TXT"]
    chk(len(hello) == 1, "HELLO.TXT is in the root directory")
    if hello:
        h = hello[0]
        chk(h["size"] == len(want), "HELLO.TXT size matches",
            "%d want %d" % (h["size"], len(want)))
        clus, term = chain(h["clus"])
        data = b"".join(cluster_bytes(c) for c in clus)[:h["size"]]
        chk(data == want, "HELLO.TXT bytes match exactly")
        chk(term >= 0x0FFFFFF8, "HELLO.TXT chain ends in an EOC marker",
            "terminator 0x%08x" % term)
        chk(h["wrt_date"] != 0, "HELLO.TXT carries a real write date",
            "0x%04x" % h["wrt_date"])

    sub = [e for e in root if e["name"].upper() == "SUBDIR"]
    chk(len(sub) == 1, "SUBDIR is in the root directory")
    if sub:
        s = sub[0]
        chk((s["attr"] & 0x10) != 0, "SUBDIR is flagged as a directory",
            "attr=0x%02x" % s["attr"])
        chk(s["size"] == 0, "SUBDIR size field is 0, as the spec requires",
            "got %d" % s["size"])
        kids = read_dir(s["clus"])
        dot    = [k for k in kids if k["raw"].rstrip() == "."]
        dotdot = [k for k in kids if k["raw"].rstrip() == ".."]
        chk(len(dot) == 1 and dot[0]["clus"] == s["clus"],
            "'.' points at SUBDIR itself")
        # The spec is specific: '..' stores 0 when the parent is the root.
        chk(len(dotdot) == 1 and dotdot[0]["clus"] == 0,
            "'..' stores 0 because the parent is the root",
            "got %d" % (dotdot[0]["clus"] if dotdot else -1))
        nested = [k for k in kids if k["name"].upper() == "NESTED.TXT"]
        chk(len(nested) == 1, "NESTED.TXT is inside SUBDIR")
        if nested:
            n = nested[0]
            clus, _t = chain(n["clus"])
            data = b"".join(cluster_bytes(c) for c in clus)[:n["size"]]
            chk(data == b"nested\n", "NESTED.TXT bytes match exactly", repr(data))

    print("")
    print("== gate")
    print("   checks: pass=%d fail=%d" % (len(oks), len(fails)))
    if len(oks) < 25:
        print("   too few checks ran (%d, expected >=25)" % len(oks))
        return 1
    print("VERDICT: " + ("PASS" if not fails else "FAIL"))
    return 1 if fails else 0

sys.exit(main())
