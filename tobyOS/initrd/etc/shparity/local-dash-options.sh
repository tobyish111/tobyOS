# `local -` makes the shell's set -o options local to the function: the
# return sets each one that changed back, shopt's options not among them,
# and `local` lists it as `local -` (finding 383). tsh said "`-': not a
# valid identifier".
exec 2>&1
f() { local -; set +o ignoreeof; shopt -o ignoreeof; }; IGNOREEOF=0; f; shopt -o ignoreeof; echo "[$IGNOREEOF]"
g() { local -; set -u -f; set +B; }; b="$-|$SHELLOPTS"; g; a="$-|$SHELLOPTS"; [ "$a" = "$b" ] && echo match || echo "differ [$b] [$a]"
h() { local -; shopt -s extglob; set -f; }; h; shopt extglob; echo "$-"
k() { local -; local x=1; local; }; k
