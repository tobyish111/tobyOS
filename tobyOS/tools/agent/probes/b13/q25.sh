exec 2>&1
set -o
echo ==; set +o
echo ==; set +B; echo {a,b}; set -B; echo {a,b}
echo ==; set -k; env x=1 sh -c 'echo "x=$x"' y=2; echo "x=${x-unset}"; set +k
echo ==; f() { echo "in f"; }; trap 'echo "DBG $BASH_COMMAND"' DEBUG; f; trap - DEBUG
echo ==; set -T; trap 'echo "DBG2 $BASH_COMMAND"' DEBUG; f; trap - DEBUG; set +T
echo ==; mkdir -p /tmp/ph/real; ln -sfn /tmp/ph/real /tmp/ph/link; cd /tmp/ph/link; pwd; set -P; cd /tmp/ph/link; pwd; set +P; cd /tmp
echo ==; echo "$-"
echo ==; sh -c 'set -t; echo one; echo two'; echo after-t
echo ==; set -o interactive-comments; echo ok-ic
echo ==; set -H; echo "$-"; set +H
echo end
