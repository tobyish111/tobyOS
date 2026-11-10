#!/bin/bash
# The GUI editor's file->buffer parse, on the HOST.
#
#   bash logs/edittrunc.sh
#
# Why a host test: gui_edit is a GUI program and cannot be driven headlessly,
# yet load_file() is exactly where a text editor loses data. It used to
# truncate three different ways (read buffer smaller than the line array, line
# cap, column cap), report "Loaded", and then let Ctrl+S write the short copy
# back over the original with O_TRUNC.
#
# textbuf.h is compiled into BOTH the editor and this test, so what runs here
# is the real parse, not a model of it. The save REFUSAL that depends on the
# `truncated` flag lives in main.c and is not covered here.
set -o pipefail
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
# NOT mktemp: that yields an MSYS /tmp/... path the NATIVE linker
# cannot resolve ("cannot open output file"). Same trap as QEMU's
# -serial file:. Build inside the tree instead.
mkdir -p build
OUT=build/tbtest.exe
if ! clang -O1 -Wall -Wextra -Werror -I programs/user_gui_edit \
        -o "$OUT" programs/user_gui_edit/textbuf_test.c; then
    echo "VERDICT: FAIL (test did not build)"; exit 1
fi
"$OUT"; rc=$?
[ $rc = 0 ] && echo "VERDICT: PASS" || echo "VERDICT: FAIL"
exit $rc
