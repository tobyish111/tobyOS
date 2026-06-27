#!/bin/bash
# Build linux-fbgui: a DYNAMICALLY-LINKED (musl) Linux x86-64 fbdev app.
#
# We have no musl sysroot, so we link directly against the real musl libc
# shared object that ships in the initrd (programs/busybox/ld-musl-x86_64.so.1,
# whose SONAME is libc.musl-x86_64.so.1 -- in musl the loader IS libc). The
# in-tree clang cross-compiles to the Linux/musl target; our own crt.c supplies
# the standard _start -> __libc_start_main entry (no Scrt1.o needed). The result
# is a PIE with PT_INTERP=/lib/ld-musl-x86_64.so.1 + DT_NEEDED
# libc.musl-x86_64.so.1, run unmodified through the genuine ld-musl loader.
set -e
cd "$(dirname "$0")/../.." || exit 1
CC=${CC:-clang}
MUSL=programs/busybox/ld-musl-x86_64.so.1
OUT=programs/linux-fbgui/linux-fbgui.elf

if [ ! -f "$MUSL" ]; then
    echo "[fbgui] musl libc ($MUSL) absent -- skipping (opt-in)"; exit 0
fi

# Stop MSYS2/Git-Bash from rewriting the absolute --dynamic-linker=/lib/... path
# into a Windows path (e.g. C:/msys64/lib/...) -- that would bake a bogus
# PT_INTERP into the ELF so the loader could never be found at runtime.
export MSYS2_ARG_CONV_EXCL="*"
export MSYS_NO_PATHCONV=1

"$CC" --target=x86_64-unknown-linux-musl -O2 -fPIC -fno-stack-protector \
      -fno-builtin -nostdlib -nostartfiles -fuse-ld=lld -pie \
      -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 \
      -Wl,-e,_start -Wl,--no-as-needed \
      -o "$OUT" \
      programs/linux-fbgui/crt.c programs/linux-fbgui/main.c \
      "$MUSL"

# Brandelf EI_OSABI=3 (Linux) so the loader runs it under the Linux personality.
printf '\003' | dd of="$OUT" bs=1 seek=7 count=1 conv=notrunc 2>/dev/null
echo "[fbgui] built $OUT"
