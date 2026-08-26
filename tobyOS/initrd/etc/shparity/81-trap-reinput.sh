# POSIX trap: `trap` with no operands prints every trap in a form the
# shell can re-read; `trap - SIG` removes one; an EXIT trap fires when the
# script ends. Signal DELIVERY is covered elsewhere -- this pins the report.
trap 'echo usr1' USR1
trap 'echo term two' TERM
trap | grep USR1
trap | grep TERM
trap - USR1
trap | grep -c USR1
trap 'echo bye' EXIT
echo end
