# FUNCNAME outside a function is declared and invisible (`declare -p
# FUNCNAME` says `declare -a FUNCNAME`, `set` and -v do not see it), and
# `unset FUNCNAME` takes it away for good; the `declare -p`/`declare -a`
# listing is sorted by name, a declared-empty array included; the dynamic
# variables print as bash's do. tsh said "not found" and listed in
# creation order.
# (batch 12; probe fn1 in C:\t\b12)
exec 2>&1
declare -p FUNCNAME; echo "s=$?"
declare -p | grep -c FUNCNAME
set | grep -c FUNCNAME
declare -p | grep FUNCNAME
echo "[${FUNCNAME[@]-unset}] [${#FUNCNAME[@]}] [${FUNCNAME-u}]"
f() { declare -p FUNCNAME; declare -p | grep FUNCNAME; }
f
declare -a | grep -c FUNCNAME
declare -p BASH_LINENO BASH_SOURCE BASH_ARGC BASH_ARGV
g() { declare -p BASH_LINENO BASH_ARGC; }; g a b
[[ -v FUNCNAME ]]; echo "v=$?"
unset FUNCNAME; declare -p FUNCNAME; echo "s=$?"
f
