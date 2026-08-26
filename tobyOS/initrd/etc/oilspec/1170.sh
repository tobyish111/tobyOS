# https://pubs.opengroup.org/onlinepubs/9699919799/utilities/V3_chap02.html#tag_18_14
#
# 2.8.1 - Consequences of shell errors
#
# Special built-ins should exit a non-interactive shell
# bash and busybox dont't implement this even with set -o posix, so it seems risky
# dash and mksh do it; so does AT&T ksh

$SH -c '
if test -n "$BASH_VERSION"; then
  set -o posix
fi
set -- a b
shift 3
echo status=$?
'
if test "$?" != 0; then
  echo 'non-zero status'
fi
