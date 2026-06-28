#!/bin/bash
# Build linux-paint: a DYNAMICALLY-LINKED (musl) Linux x86-64 interactive
# graphical app -- draws to /dev/fb0 and reads the mouse from
# /dev/input/event1. Same build shape as programs/linux-fbgui (no musl
# sysroot; link against the real musl libc .so in the initrd, with our own
# crt.c standing in for Scrt1.o). Result: a PIE with
# PT_INTERP=/lib/ld-musl-x86_64.so.1, run unmodified through ld-musl.
set -e
cd "$(dirname "$0")/../.." || exit 1
CC=${CC:-clang}
MUSL=programs/busybox/ld-musl-x86_64.so.1
OUT=programs/linux-paint/linux-paint.elf

if [ ! -f "$MUSL" ]; then
    echo "[paint] musl libc ($MUSL) absent -- skipping (opt-in)"; exit 0
fi

export MSYS2_ARG_CONV_EXCL="*"
export MSYS_NO_PATHCONV=1

"$CC" --target=x86_64-unknown-linux-musl -O2 -fPIC -fno-stack-protector \
      -fno-builtin -nostdlib -nostartfiles -fuse-ld=lld -pie \
      -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 \
      -Wl,-e,_start -Wl,--no-as-needed \
      -o "$OUT" \
      programs/linux-paint/crt.c programs/linux-paint/main.c \
      "$MUSL"

printf '\003' | dd of="$OUT" bs=1 seek=7 count=1 conv=notrunc 2>/dev/null
echo "[paint] built $OUT"
