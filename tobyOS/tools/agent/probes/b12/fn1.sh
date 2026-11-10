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
