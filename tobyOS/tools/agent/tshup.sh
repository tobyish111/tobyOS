#!/bin/bash
# tshup.sh [ELF] -- replace /bin/tsh AND /bin/sh on the running guest ($ED_HOST,
# $ED_PORT, $ED_USER as edremote.sh takes them) with ELF (default
# programs/tsh/tsh.elf), so a hosted-shell change is tested without an ISO
# rebuild or reboot. The SSH channel truncates large uploads silently at a
# boundary that moves (memory: hw-upload-without-reflash), so the guest's md5
# is checked against the local one and the upload retried. Proves the HOSTED
# shell only: the kernel's copy of src/shell.c needs a new ISO.
cd "$(dirname "$0")/../.." || exit 1
ELF=${1:-programs/tsh/tsh.elf}
want=$(md5sum < "$ELF" | cut -c1-32)
for try in 1 2 3 4 5; do
  bash logs/edremote.sh "cat > /tmp/tsh.up" < "$ELF" >/dev/null 2>&1
  got=$(bash logs/edremote.sh "md5sum /tmp/tsh.up" 2>/dev/null | tr -d '\r' | cut -c1-32)
  if [ "$got" = "$want" ]; then
    bash logs/edremote.sh "cp /tmp/tsh.up /bin/tsh && cp /tmp/tsh.up /bin/sh && chmod 755 /bin/tsh /bin/sh && md5sum /bin/tsh /bin/sh" 2>/dev/null | tr -d '\r'
    echo "tshup: $ELF ($want) installed, try $try"
    exit 0
  fi
  echo "tshup: try $try got '$got' want $want -- retrying" >&2
done
echo "tshup: FAILED" >&2
exit 1
