#!/bin/bash
# stress.sh TAG -- shparity with a SHELL_SB_STRESS build of src/shell.c (every
# shell_sb growth moves the text and poisons the old block, so a stale pointer
# reads garbage at once). Fixture 155 is set aside: its $(seq 200000) capture is
# quadratic in this build by design and takes hours. shell.c is backed up HERE
# and restored from that backup -- never from an older "final" copy, which once
# almost reverted a finished change. Afterwards shell.c is touched so make rebuilds.
cd "$(dirname "$0")/../.." || exit 1
TAG=${1:-stress}
BK=$(mktemp -d)
cp src/shell.c "$BK/shell.c" || exit 1
mv initrd/etc/shparity/155-program-arguments.sh "$BK/" || exit 1
trap 'cp "$BK/shell.c" src/shell.c; touch src/shell.c; mv "$BK/155-program-arguments.sh" initrd/etc/shparity/ 2>/dev/null' EXIT
python - <<'PY'
p='src/shell.c'
t=open(p,encoding='utf-8',newline='').read()
assert not t.startswith('#define SHELL_SB_STRESS')
nl='\r\n' if '\r\n' in t[:5000] else '\n'
open(p,'w',encoding='utf-8',newline='').write('#define SHELL_SB_STRESS 1'+nl+t)
PY
git checkout -- initrd/etc/oilspec/FILTER 2>/dev/null
bash logs/shparity.sh > logs/gate-$TAG-shparity.log 2>&1
taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1
grep -a "VERDICT" logs/gate-$TAG-shparity.log
echo "panics=$(grep -ac 'KERNEL PANIC' logs/gate-$TAG-shparity.log)"
echo "STRESS DONE (155 set aside)"
