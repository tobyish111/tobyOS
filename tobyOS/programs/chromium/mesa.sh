#!/usr/bin/env bash
# mesa.sh -- stage Mesa's virgl GL stack into the chrome sysroot
# (tier 3 Phase 1c).
#
# Slice 98 gave the guest a real DRM render node (/dev/dri/renderD128)
# that drives host virglrenderer -- proven by the DRMTEST guard. Nothing
# in userspace speaks to it yet, because the guest has no GL
# implementation: chrome reaches a GPU only as
#
#     chrome -> EGL -> Mesa (gallium/virgl) -> libdrm -> ioctl(/dev/dri)
#
# and only libdrm/libgbm were staged (they came in as chrome's own
# DT_NEEDED, not as a working driver).
#
# This fetches the Debian bookworm Mesa runtime -- the SAME trade the
# arc already made for chrome's 60+ DSOs: use a real distro build rather
# than port a stack. Kept in its OWN script (not sysroot.sh) so a GPU
# experiment can never destabilise the browser payload that works today.
#
# Deliberately NOT run by the Makefile: staging it grows the initrd and
# changes what chrome dlopens. Run it when you intend to test GL.
#
#   bash programs/chromium/mesa.sh
#   (then rebuild; chrome needs --use-gl=egl to actually try it)
set -euo pipefail
cd "$(dirname "$0")"
OUT="sysroot"
MIRROR="https://deb.debian.org/debian"
SUITE="${SUITE:-bookworm}"

# libgl1-mesa-dri   : the gallium DRI drivers, incl. virtio_gpu/virgl
# libegl-mesa0      : Mesa's EGL platform (what chrome's --use-gl=egl loads)
# libegl1/libgles2  : the vendor-neutral loaders chrome links against
# libglapi-mesa     : shared GL dispatch
# libdrm2/libgbm1   : already present, listed so the closure is explicit
# libz/libzstd/libexpat/libwayland*: Mesa's own deps
# libglvnd (libGLdispatch/libGLX) is the vendor-neutral dispatch layer
# Debian's libEGL.so.1 is a member of -- the FIRST gap-list entry this
# staging produced was its missing libGLdispatch.so.0.
SEED="libgl1-mesa-dri libegl-mesa0 libegl1 libgles2 libglapi-mesa \
libglvnd0 libglx0 libgl1 libopengl0 libxcb-glx0 libx11-xcb1 \
libxcb-randr0 libxcb-shm0 libxcb-xkb1 libxcb-render0 libxcb-util1 \
libdrm2 libgbm1 libdrm-amdgpu1 libdrm-intel1 libdrm-nouveau2 libdrm-radeon1 \
libzstd1 libelf1 libwayland-client0 libwayland-server0 libxcb-dri2-0 \
libxcb-dri3-0 libxcb-present0 libxcb-sync1 libxcb-xfixes0 libxshmfence1"

mkdir -p "$OUT" .deb-cache
echo "[mesa] fetching Packages index ($SUITE) ..."
[ -f .deb-cache/Packages ] || {
    curl -# -m 300 -o .deb-cache/Packages.xz \
        "$MIRROR/dists/$SUITE/main/binary-amd64/Packages.xz"
    xz -df .deb-cache/Packages.xz
}

python - "$OUT" "$MIRROR" "$SEED" <<'PY'
import sys, os, urllib.request, subprocess, glob, shutil
out, mirror, seed = sys.argv[1], sys.argv[2], sys.argv[3].split()
pkgs = {}
cur = {}
for line in open('.deb-cache/Packages', encoding='utf-8', errors='replace'):
    if line.startswith('Package: '):
        cur = {'name': line[9:].strip()}
        pkgs[cur['name']] = cur
    elif line.startswith('Filename: '):
        cur['fn'] = line[10:].strip()

os.makedirs('.deb-cache', exist_ok=True)
staged = []
for name in seed:
    p = pkgs.get(name)
    if not p or 'fn' not in p:
        print('[mesa] MISSING from index: %s' % name)
        continue
    deb = os.path.join('.deb-cache', os.path.basename(p['fn']))
    if not os.path.exists(deb):
        print('[mesa] fetching %s' % name)
        urllib.request.urlretrieve(mirror + '/' + p['fn'], deb)
    staged.append(deb)

# Unpack with the same pure-python .deb reader sysroot.sh uses (this
# host has no dpkg-deb/ar). Mesa's DRI drivers live under
# .../dri/ and are dlopen'd BY PATH, so they keep that subdirectory;
# everything else flattens by basename like the rest of the sysroot.
import io, lzma, tarfile, gzip

def ar_members(data):
    assert data[:8] == b'!<arch>\n', 'not an ar archive'
    off = 8
    while off < len(data):
        hdr = data[off:off+60]; off += 60
        if len(hdr) < 60: break
        name = hdr[0:16].decode().strip()
        size = int(hdr[48:58].decode().strip())
        body = data[off:off+size]; off += size + (size & 1)
        yield name, body

os.makedirs(os.path.join(out, 'dri'), exist_ok=True)
nso = ndri = nlink = 0
pending = []            # links whose target had not been extracted yet
for deb in staged:
    data = open(deb, 'rb').read()
    for name, body in ar_members(data):
        if not name.startswith('data.tar'):
            continue
        if name.endswith('.xz'):    raw = lzma.decompress(body)
        elif name.endswith('.gz'):  raw = gzip.decompress(body)
        elif name.endswith('.zst'):
            print('  WARN zstd data.tar in %s -- skipped' % deb); continue
        else:                        raw = body
        tf = tarfile.open(fileobj=io.BytesIO(raw))
        for m in tf.getmembers():
            p = m.name.lstrip('./')
            base = os.path.basename(p)
            if '.so' not in base:
                continue
            is_dri = '/dri/' in ('/' + p)
            dstdir = os.path.join(out, 'dri') if is_dri else out
            if m.isfile():
                with open(os.path.join(dstdir, base), 'wb') as f:
                    f.write(tf.extractfile(m).read())
                if is_dri: ndri += 1
                else:      nso += 1
            elif m.issym() or m.islnk():
                # Debian ships every gallium driver as a HARDLINK to one
                # blob (virtio_gpu_dri.so, crocus_dri.so, ... are the same
                # file). tarfile reports those as LNKTYPE, not regular
                # files -- ignoring them staged exactly ONE driver and
                # silently lost virgl, the only one we actually need.
                tgt = os.path.basename(m.linkname)
                src = os.path.join(dstdir, tgt)
                dst = os.path.join(dstdir, base)
                if os.path.exists(src) and not os.path.exists(dst):
                    shutil.copyfile(src, dst); nlink += 1
                elif not os.path.exists(src):
                    pending.append((dstdir, base, tgt))
# Second pass: links that appeared before their target in the archive.
for dstdir, base, tgt in pending:
    src = os.path.join(dstdir, tgt)
    dst = os.path.join(dstdir, base)
    if os.path.exists(src) and not os.path.exists(dst):
        shutil.copyfile(src, dst); nlink += 1
print('[mesa] staged %d libs + %d DRI drivers (+%d link copies) into %s'
      % (nso, ndri, nlink, out))
# PRUNE. Debian hardlinks all 13 gallium drivers to one 25 MB blob, so
# faithfully materialising them costs ~330 MB -- on top of chrome's 262 MB
# in the same RAM-backed initrd. We drive virtio-gpu and nothing else;
# keep virgl (+ swrast as the software fallback Mesa picks when the DRM
# node is missing) and drop the rest.
KEEP_DRI = ('virtio_gpu_dri.so', 'swrast_dri.so')
freed = 0
for f in os.listdir(os.path.join(out, 'dri')):
    if f not in KEEP_DRI:
        pth = os.path.join(out, 'dri', f)
        freed += os.path.getsize(pth)
        os.remove(pth)
print('[mesa] pruned unused DRI drivers (%d MB freed); kept %s'
      % (freed // (1024*1024), ', '.join(KEEP_DRI)))

for want in ('dri/virtio_gpu_dri.so', 'libEGL_mesa.so.0', 'libgbm.so.1'):
    print('[mesa] %-28s %s' % (want,
          'OK' if os.path.exists(os.path.join(out, want)) else 'MISSING'))
PY

echo "[mesa] sysroot now:"
ls -la "$OUT"/libEGL* "$OUT"/libGL* "$OUT"/dri/ 2>/dev/null | head -20
du -sh "$OUT"
