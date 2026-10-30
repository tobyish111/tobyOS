# declare -f -p of a name that is no function says "declare: f1: not
# found", status 1; without -p, and with -F, it is silent (finding 428).
exec 2>&1
f0() { :; }; declare -f -p f1 f0 f2; echo "rc=$?"; declare -fp f3; echo "rc=$?"
declare -f g1; echo "rc=$?"; declare -F g2; echo "rc=$?"; typeset -fp g3; echo "rc=$?"
