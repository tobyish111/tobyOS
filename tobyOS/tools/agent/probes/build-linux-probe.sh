#!/bin/bash
# build-linux-probe.sh SRC.c OUT -- build a raw-syscall Linux static ELF the way the
# Makefile builds programs/linux-hello, and stamp EI_OSABI=3 so tobyOS runs it under
# the Linux personality. Upload with: bash logs/edremote.sh 'cat > /tmp/x; chmod +x /tmp/x' < OUT
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
TMP='C:\t' TEMP='C:\t' clang --target=x86_64-unknown-linux-gnu -static -nostdlib -ffreestanding \
  -fno-stack-protector -fno-pic -fno-pie -O2 -fuse-ld=lld -Wl,-e,_start -o "$2" "$1" &&
printf '\003' | dd of="$2" bs=1 seek=7 count=1 conv=notrunc 2>/dev/null
