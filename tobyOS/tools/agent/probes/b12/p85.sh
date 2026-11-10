exec 2>&1
declare -a xx
declare -a | grep -c ' xx$'
declare -p | grep ' xx'
set | grep -c '^xx'
[[ -v xx ]]; echo "v=$?"
echo "${#xx[@]} ${xx[@]-unset} ${xx-u}"
zz=1; declare -p | grep -n ' xx\| zz=' | sed 's/:.*declare/ declare/'
f() { local -a yy; declare -p | grep ' yy'; }; f
