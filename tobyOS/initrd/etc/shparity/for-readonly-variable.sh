# A for loop over a readonly variable says "VAR: readonly variable" and no
# more -- tsh added "for: failed to set 'VAR'" (finding 416).
exec 2>&1
declare -r VAR=1; for VAR in 1 2 3; do echo $VAR; done; echo "rc=$?"
f() { local -r L=1; for L in a; do echo $L; done; echo "f rc=$?"; }; f
