# FUNCNAME outside a function is declared and invisible (`declare -p
# FUNCNAME` says `declare -a FUNCNAME`, `set` and -v do not see it), and
# `unset FUNCNAME` takes it away for good; the `declare -p`/`declare -a`
# listing is sorted by name, a declared-empty array included; the dynamic
# variables print as bash's do. tsh said "not found" and listed in
# creation order.
# (batch 12; probe p85 in C:\t\b12)
exec 2>&1
declare -a xx
declare -a | grep -c ' xx$'
declare -p | grep ' xx'
set | grep -c '^xx'
[[ -v xx ]]; echo "v=$?"
echo "${#xx[@]} ${xx[@]-unset} ${xx-u}"
zz=1; declare -p | grep -n ' xx\| zz=' | sed 's/:.*declare/ declare/'
f() { local -a yy; declare -p | grep ' yy'; }; f
