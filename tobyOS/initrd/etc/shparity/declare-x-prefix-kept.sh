# A prefix assignment before declare -x stays, exported, as one before
# export does; before declare -p it does not, and in a function -x makes a
# local (finding 429).
exec 2>&1
FOO='$$' declare -x FOO; declare -p FOO; unset FOO
FOO=1 declare -p FOO; echo "rc=$?"; declare -p FOO; echo "rc=$?"
BAR=2 declare -rx BAR; declare -p BAR
f() { Z=3 declare -x Z; declare -p Z; }; f; declare -p Z; echo "rc=$?"
