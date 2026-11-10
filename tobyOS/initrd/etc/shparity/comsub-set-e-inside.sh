# A command substitution does not inherit -e, but a set -e inside it holds
# there: x=$(set -e; false; echo bad) stops at the false. In posix mode the
# substitution inherits -e. tsh kept -e off for the whole substitution
# (finding 435).
exec 2>&1
x=$(set -e; false; echo bad); echo "$? [$x]"
set -e; x=$(false; echo ran); echo "[$x]"; echo "[$(echo $- | tr -dc e)]"; set +e
"$BASH" -c 'set -o posix; set -e; z=$(false; echo posix foo); echo $z'; echo "rc=$?"
"$BASH" -c 'set -e; z=$(false; echo non-posix foo); echo $z'; echo "rc=$?"
"$BASH" -c 'shopt -s inherit_errexit; set -e; x=$(false; echo ran2); echo "[$x]"'; echo "rc=$?"
