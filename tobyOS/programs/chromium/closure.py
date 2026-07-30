#!/usr/bin/env python3
"""Slice 69: close the DSO dependency graph for the FULL chrome binary.

Discovering missing libraries one guest boot at a time costs ~7 minutes per
library (chrome's loader reports only the FIRST failure). This walks
DT_NEEDED across the chrome binary and every .so already staged in
sysroot/, subtracts what we have (plus what the in-tree glibc provides),
maps the remaining sonames to Debian packages, fetches and stages them, and
repeats until the closure is empty -- offline, in seconds.

Usage: python programs/chromium/closure.py [--apply]
       (without --apply it only reports what is missing)
"""
import glob
import gzip
import io
import lzma
import os
import shutil
import struct
import sys
import tarfile
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
os.chdir(HERE)
SYSROOT = 'sysroot'
CACHE = '.deb-cache'
MIRROR = 'https://deb.debian.org/debian'

# Provided by the in-tree Bootlin glibc (never staged -- see sysroot.sh).
GLIBC = {
    'libc.so.6', 'libm.so.6', 'libdl.so.2', 'libpthread.so.0', 'librt.so.1',
    'libresolv.so.2', 'libutil.so.1', 'libnsl.so.1', 'libanl.so.1',
    'ld-linux-x86-64.so.2', 'libgcc_s.so.1', 'libcrypt.so.1',
}

# soname -> Debian package. Extend as the closure surfaces new ones.
PKG = {
    'libgssapi_krb5.so.2': 'libgssapi-krb5-2', 'libkrb5.so.3': 'libkrb5-3',
    'libk5crypto.so.3': 'libk5crypto3', 'libcom_err.so.2': 'libcom-err2',
    'libkrb5support.so.0': 'libkrb5support0', 'libkeyutils.so.1': 'libkeyutils1',
    'libssl.so.3': 'libssl3', 'libcrypto.so.3': 'libssl3',
    'libgnutls.so.30': 'libgnutls30', 'libtasn1.so.6': 'libtasn1-6',
    'libnettle.so.8': 'libnettle8', 'libhogweed.so.6': 'libhogweed6',
    'libgmp.so.10': 'libgmp10', 'libp11-kit.so.0': 'libp11-kit0',
    'libidn2.so.0': 'libidn2-0', 'libunistring.so.2': 'libunistring2',
    'libffi.so.8': 'libffi8', 'libzstd.so.1': 'libzstd1', 'libz.so.1': 'zlib1g',
    'liblzma.so.5': 'liblzma5', 'libbz2.so.1.0': 'libbz2-1.0',
    'libbrotlidec.so.1': 'libbrotli1', 'libbrotlicommon.so.1': 'libbrotli1',
    'libpng16.so.16': 'libpng16-16', 'libfreetype.so.6': 'libfreetype6',
    'libfontconfig.so.1': 'libfontconfig1', 'libexpat.so.1': 'libexpat1',
    'libuuid.so.1': 'libuuid1', 'libblkid.so.1': 'libblkid1',
    'libmount.so.1': 'libmount1', 'libselinux.so.1': 'libselinux1',
    'libpcre2-8.so.0': 'libpcre2-8-0', 'libsystemd.so.0': 'libsystemd0',
    'libcap.so.2': 'libcap2', 'libgcrypt.so.20': 'libgcrypt20',
    'libgpg-error.so.0': 'libgpg-error0', 'libdbus-1.so.3': 'libdbus-1-3',
    'libglib-2.0.so.0': 'libglib2.0-0', 'libgobject-2.0.so.0': 'libglib2.0-0',
    'libgio-2.0.so.0': 'libglib2.0-0', 'libgmodule-2.0.so.0': 'libglib2.0-0',
    'libcairo-gobject.so.2': 'libcairo-gobject2', 'libpixman-1.so.0': 'libpixman-1-0',
    'libxcb-shm.so.0': 'libxcb-shm0', 'libxcb-render.so.0': 'libxcb-render0',
    'libXrender.so.1': 'libxrender1', 'libharfbuzz.so.0': 'libharfbuzz0b',
    'libgraphite2.so.3': 'libgraphite2-3', 'libfribidi.so.0': 'libfribidi0',
    'libthai.so.0': 'libthai0', 'libdatrie.so.1': 'libdatrie1',
    'libavahi-client.so.3': 'libavahi-client3',
    'libavahi-common.so.3': 'libavahi-common3',
    'libXau.so.6': 'libxau6', 'libXdmcp.so.6': 'libxdmcp6',
    'libbsd.so.0': 'libbsd0', 'libmd.so.0': 'libmd0',
    'libnss3.so': 'libnss3', 'libnssutil3.so': 'libnss3',
    'libsmime3.so': 'libnss3', 'libplc4.so': 'libnspr4',
    'libplds4.so': 'libnspr4', 'libnspr4.so': 'libnspr4',
}


def needed(path):
    """DT_NEEDED sonames of an ELF64 file (empty set if not ELF)."""
    try:
        d = open(path, 'rb').read()
    except OSError:
        return set()
    if d[:4] != b'\x7fELF' or len(d) < 0x40 or d[4] != 2:
        return set()
    e_shoff = struct.unpack_from('<Q', d, 0x28)[0]
    e_shentsize = struct.unpack_from('<H', d, 0x3a)[0]
    e_shnum = struct.unpack_from('<H', d, 0x3c)[0]
    if e_shoff == 0 or e_shoff + e_shnum * e_shentsize > len(d):
        return set()
    secs, dyn = [], None
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_type = struct.unpack_from('<I', d, off + 4)[0]
        sh_off = struct.unpack_from('<Q', d, off + 0x18)[0]
        sh_size = struct.unpack_from('<Q', d, off + 0x20)[0]
        sh_link = struct.unpack_from('<I', d, off + 0x28)[0]
        secs.append((sh_type, sh_off, sh_size, sh_link))
        if sh_type == 6:
            dyn = (sh_off, sh_size, sh_link)
    if not dyn:
        return set()
    sh_off, sh_size, sh_link = dyn
    st_off = secs[sh_link][1]
    out = set()
    for o in range(sh_off, min(sh_off + sh_size, len(d) - 16), 16):
        tag, val = struct.unpack_from('<QQ', d, o)
        if tag == 0:
            break
        if tag == 1:
            e = d.index(b'\0', st_off + val)
            out.add(d[st_off + val:e].decode())
    return out


def load_index():
    pkgs, cur = {}, {}
    p = os.path.join(CACHE, 'Packages')
    for line in open(p, encoding='utf-8', errors='replace'):
        if line.startswith('Package: '):
            cur = {'name': line[9:].strip()}
        elif line.startswith('Filename: '):
            cur['fn'] = line[10:].strip()
        elif line.strip() == '' and cur.get('name'):
            pkgs[cur['name']] = cur
            cur = {}
    return pkgs


def stage_deb(deb):
    n = 0
    data = open(deb, 'rb').read()
    assert data[:8] == b'!<arch>\n'
    off = 8
    while off < len(data):
        hdr = data[off:off + 60]
        off += 60
        if len(hdr) < 60:
            break
        name = hdr[0:16].decode().strip()
        size = int(hdr[48:58].decode().strip())
        body = data[off:off + size]
        off += size + (size & 1)
        if not name.startswith('data.tar'):
            continue
        raw = (lzma.decompress(body) if name.endswith('.xz')
               else gzip.decompress(body) if name.endswith('.gz') else body)
        tf = tarfile.open(fileobj=io.BytesIO(raw))
        for m in tf.getmembers():
            rel = m.name.lstrip('./')
            base = os.path.basename(rel)
            if '.so' not in base:
                continue
            if not ('/lib/' in '/' + rel or rel.startswith('usr/lib/')
                    or rel.startswith('lib/')):
                continue
            if m.isfile():
                open(os.path.join(SYSROOT, base), 'wb').write(
                    tf.extractfile(m).read())
                n += 1
            elif m.issym():
                tgt = os.path.join(SYSROOT, os.path.basename(m.linkname))
                dst = os.path.join(SYSROOT, base)
                if os.path.exists(tgt) and not os.path.exists(dst):
                    shutil.copyfile(tgt, dst)
    return n


def main():
    apply = '--apply' in sys.argv
    pkgs = load_index()
    roots = ['chrome-linux64/chrome']
    for rnd in range(1, 8):
        have = {os.path.basename(p) for p in glob.glob(os.path.join(SYSROOT, '*'))}
        want = set()
        for r in roots:
            want |= needed(r)
        for so in glob.glob(os.path.join(SYSROOT, '*.so*')):
            want |= needed(so)
        missing = sorted(want - have - GLIBC)
        if not missing:
            print(f'[closure] round {rnd}: CLOSED ({len(have)} libs staged)')
            return 0
        print(f'[closure] round {rnd}: {len(missing)} missing: '
              + ' '.join(missing[:12]) + ('...' if len(missing) > 12 else ''))
        if not apply:
            return 1
        unmapped = [m for m in missing if m not in PKG]
        if unmapped:
            print('[closure] NO PACKAGE MAPPING for:', ' '.join(unmapped))
        todo = {PKG[m] for m in missing if m in PKG}
        if not todo:
            return 1
        for name in sorted(todo):
            p = pkgs.get(name)
            if not p or not p.get('fn'):
                print('  [skip] no such package:', name)
                continue
            url = MIRROR + '/' + p['fn']
            deb = os.path.join(CACHE, os.path.basename(url))
            if not os.path.exists(deb):
                urllib.request.urlretrieve(url, deb)
            k = stage_deb(deb)
            print(f'  [stage] {name}: {k} objects')
    print('[closure] did not converge')
    return 1


if __name__ == '__main__':
    sys.exit(main())
