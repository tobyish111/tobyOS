#!/bin/bash
# Slice 39 FRONT C: rebuild the iso with the TKAPP harness pointed at
# /bin/chromewin (chromium-in-a-TobyTK-window over --remote-debugging-pipe).
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t
export TMP='C:\t' TEMP='C:\t' TMPDIR='C:\t'
cd /c/CustomOS/tobyOS || exit 1
touch src/kernel.c
rm -f build/initrd.tar build/base.iso tobyOS.iso
# CHROMIUM_BOOT is ALSO defined for its instruments (bt_dump_group, [chan],
# waitt labels); the console dump-dom harness is excluded when TKAPP_BOOT is
# set (see the gate at the top of the CHROMIUM block in kernel.c).
# The define set changed vs slice 38 -- touch EVERY kernel source so all
# objects rebuild with one coherent define set (prime-directive lesson).
touch src/*.c
make "CC=TMP='C:\\t' TEMP='C:\\t' clang" "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" \
     iso EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DCHROMIUM_BOOT -DTKAPP_BOOT -DTKAPP_CHROMEWIN"
