#!/bin/bash
# Build linux-evdev: a DYNAMICALLY-LINKED (musl) Linux x86-64 evdev reader.
# Same build shape as programs/linux-fbgui: no musl sysroot, so we link
# directly against the real musl libc shared object in the initrd
# (programs/busybox/ld-musl-x86_64.so.1, SONAME libc.musl-x86_64.so.1), with
# our own crt.c standing in for Scrt1.o. Result: a PIE with
# PT_INTERP=/lib/ld-musl-x86_64.so.1, run unmodified through the ld-musl loader.
set -e
cd "$(dirname "$0")/../.." || exit 1
CC=${CC:-clang}
MUSL=programs/busybox/ld-musl-x86_64.so.1
OUT=programs/linux-evdev/linux-evdev.elf

if [ ! -f "$MUSL" ]; then
    echo "[evdev] musl libc ($MUSL) absent -- skipping (opt-in)"; exit 0
fi

# Stop MSYS2/Git-Bash rewriting the absolute --dynamic-linker=/lib/... path.
export MSYS2_ARG_CONV_EXCL="*"
export MSYS_NO_PATHCONV=1

"$CC" --target=x86_64-unknown-linux-musl -O2 -fPIC -fno-stack-protector \
      -fno-builtin -nostdlib -nostartfiles -fuse-ld=lld -pie \
      -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 \
      -Wl,-e,_start -Wl,--no-as-needed \
      -o "$OUT" \
      programs/linux-evdev/crt.c programs/linux-evdev/main.c \
      "$MUSL"

printf '\003' | dd of="$OUT" bs=1 seek=7 count=1 conv=notrunc 2>/dev/null
echo "[evdev] built $OUT"
