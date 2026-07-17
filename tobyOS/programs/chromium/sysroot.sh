#!/usr/bin/env bash
# sysroot.sh -- assemble the Chromium shared-library sysroot (Tier 1 of the
# M0 gap list) by fetching Debian bookworm .debs and flattening their .so's
# into programs/chromium/sysroot/. That dir is auto-staged to /opt/chrome/sysroot
# by the Makefile; chrome finds it via LD_LIBRARY_PATH=/opt/chrome/sysroot.
#
# Instrument-first: seed with chrome-headless-shell's direct DT_NEEDED packages;
# the resolver walks Depends to close the graph. If a boot's ld.so still names a
# missing .so, add its package to SEED and re-run. Debian bookworm (glibc 2.36)
# matches chrome-for-testing's build sysroot; our libc.so.6 is 2.41 (forward-
# compatible), so glibc-provided libs (libc6/libgcc-s1) are intentionally NOT
# staged -- the in-tree Bootlin glibc provides them.
set -euo pipefail
cd "$(dirname "$0")"
OUT="sysroot"
MIRROR="https://deb.debian.org/debian"
SUITE="${SUITE:-bookworm}"
PKGURL="$MIRROR/dists/$SUITE/main/binary-amd64/Packages.xz"

# Direct DT_NEEDED (minus glibc) -> Debian package names, + a few known deps.
SEED="libglib2.0-0 libnspr4 libnss3 libatk1.0-0 libatk-bridge2.0-0 \
libatspi2.0-0 libdbus-1-3 libexpat1 libx11-6 libxcomposite1 libxdamage1 \
libxext6 libxfixes3 libxrandr2 libxcb1 libxkbcommon0 libgbm1 libudev1 \
libasound2 fontconfig libfreetype6"

mkdir -p "$OUT" .deb-cache
echo "[sysroot] fetching Packages index ($SUITE) ..."
[ -f .deb-cache/Packages ] || { curl -# -m 300 -o .deb-cache/Packages.xz "$PKGURL"; xz -df .deb-cache/Packages.xz; }

echo "[sysroot] resolving dependency closure ..."
python - "$OUT" "$MIRROR" "$SEED" <<'PY'
import sys, os, urllib.request
out, mirror, seed = sys.argv[1], sys.argv[2], sys.argv[3].split()
# Parse Packages: map name -> {Filename, Depends}
pkgs={}
cur={}
for line in open('.deb-cache/Packages', encoding='utf-8', errors='replace'):
    if line.startswith('Package: '): cur={'name':line[9:].strip()}
    elif line.startswith('Filename: '): cur['fn']=line[10:].strip()
    elif line.startswith('Depends: '): cur['dep']=line[9:].strip()
    elif line.strip()=='' and cur.get('name'):
        pkgs[cur['name']]=cur; cur={}
if cur.get('name'): pkgs[cur['name']]=cur
def deps(name):
    d=pkgs.get(name,{}).get('dep','')
    for grp in d.split(','):
        grp=grp.strip()
        if not grp: continue
        alt=grp.split('|')[0].strip()          # take first alternative
        p=alt.split()[0].strip()               # drop version constraint
        yield p
# BFS closure, skipping glibc (in-tree) + non-lib base packages
SKIP={'libc6','libgcc-s1','libc-bin','gcc-12-base','dpkg','install-info'}
seen=set(); q=list(seed); order=[]
while q:
    n=q.pop(0)
    if n in seen or n in SKIP: continue
    if n not in pkgs:  # virtual/unknown -> skip, ld.so will name it if needed
        continue
    seen.add(n); order.append(n)
    for dp in deps(n):
        if dp not in seen: q.append(dp)
print(f"[sysroot] closure = {len(order)} packages")
os.makedirs('.deb-cache', exist_ok=True)
urls=[]
for n in order:
    fn=pkgs[n].get('fn')
    if fn: urls.append((n, mirror+'/'+fn))
open('.deb-cache/urls.txt','w',newline='').write('\n'.join(u for _,u in urls)+'\n')
print('\n'.join(n for n,_ in urls))
PY

echo "[sysroot] downloading .debs ..."
while read -r U; do
    [ -z "$U" ] && continue
    B="$(basename "$U")"
    [ -f ".deb-cache/$B" ] || curl -# -m 300 -o ".deb-cache/$B" "$U"
done < .deb-cache/urls.txt

echo "[sysroot] extracting .so files ..."
python - "$OUT" <<'PY'
import sys, os, io, glob, lzma, tarfile
out=sys.argv[1]
def ar_members(data):
    assert data[:8]==b'!<arch>\n', 'not an ar archive'
    off=8
    while off < len(data):
        hdr=data[off:off+60]; off+=60
        if len(hdr)<60: break
        name=hdr[0:16].decode().strip()
        size=int(hdr[48:58].decode().strip())
        body=data[off:off+size]; off+=size + (size&1)
        yield name, body
n=0
for deb in sorted(glob.glob('.deb-cache/*.deb')):
    data=open(deb,'rb').read()
    for name, body in ar_members(data):
        if name.startswith('data.tar'):
            if name.endswith('.xz'): raw=lzma.decompress(body)
            elif name.endswith('.gz'):
                import gzip; raw=gzip.decompress(body)
            elif name.endswith('.zst'):
                print('  WARN zstd data.tar in',deb,'-- install zstd or use bookworm'); continue
            else: raw=body
            tf=tarfile.open(fileobj=io.BytesIO(raw))
            for m in tf.getmembers():
                p=m.name.lstrip('./')
                # take any regular .so* under lib dirs (flatten by basename)
                if ('/lib/' in '/'+p or p.startswith('lib/') or p.startswith('usr/lib/')) and \
                   ('.so' in os.path.basename(p)) and m.isfile():
                    base=os.path.basename(p)
                    dst=os.path.join(out, base)
                    with open(dst,'wb') as f: f.write(tf.extractfile(m).read())
                    n+=1
                # symlinks (soname -> real) -- recreate as copies of the target basename
                if ('.so' in os.path.basename(p)) and m.issym():
                    tgt=os.path.basename(m.linkname)
                    src=os.path.join(out, tgt)
                    dst=os.path.join(out, os.path.basename(p))
                    if os.path.exists(src) and not os.path.exists(dst):
                        import shutil; shutil.copyfile(src, dst)
print(f"[sysroot] extracted {n} .so files into {out}/")
PY
ls "$OUT" | wc -l
echo "[sysroot] done. Rebuild: bash logs/chromium-m0.sh (initrd re-tars with /opt/chrome/sysroot)"
