# read, printf -v, declare and local check an expanded NAME[SUB] as bash
# does: its subscript ends at the `]` that closes it, quotes, escapes and
# nested brackets counted, and that `]` ends the word -- `A[8's]`, `A[a]b]`
# are "not a valid identifier"; read goes no further after one (finding
# 367). tsh set the keys 8s and a]b.
exec 2>&1
declare -A A; v=Z
for k in '$v' "8's" 'a]b' '\' '[a' '@'; do read A[$k] <<< r; echo "[$k] $?"; done; declare -p A
declare -A P; for k in "8's" 'a]b'; do printf -v P[$k] %s p; echo "[$k] $?"; done; declare -p P
declare -A L; declare L["foo[bar"]=bleh; echo "rc=$?"; typeset L["foo]bar"]=bax; echo "rc=$?"; declare -p L
k='$(echo X)'; read A[$k] B <<< "1 2"; echo "rc=$? [${B-unset}]"
