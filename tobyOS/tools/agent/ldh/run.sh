#!/bin/bash
# run.sh -- build tools/agent/ldh/ldh.c against src/shell_ldouble.inc in WSL
# and run it: printf's long double conversions against glibc's strtold and
# printf("%L..."). glibc's %#.2g of 99.995 says "1.e+02" where C, musl and
# the guest's bash say "1.0e+02": those four lines are expected.
cd "$(dirname "$0")" || exit 1
cp ../../../src/shell_ldouble.inc . || exit 1
wsl -e bash -c 'cd "$(wslpath "'"$(pwd -W 2>/dev/null || pwd)"'")" && gcc -O1 -Wall -Wextra -o /tmp/ldh ldh.c -lm && /tmp/ldh | tail -20'
rm -f shell_ldouble.inc
