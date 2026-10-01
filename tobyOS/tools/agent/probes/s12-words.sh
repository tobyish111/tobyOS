# S12 wording: each builtin error the shparity corpus showed differing,
# one per line, stderr merged (probe2.sh). Both shells run THIS path, so
# the "SCRIPT: line N: " prefix compares as-is.
cd /tmp; rm -rf w12.$$; mkdir w12.$$; cd w12.$$; : > afile
cd nosuchdir; echo "cd missing=$?"
cd afile; echo "cd file=$?"
( unset OLDPWD; cd - ); echo "cd - unset=$?"
alias nosuch_al; echo "alias=$?"
unalias nosuch_al; echo "unalias=$?"
set -- a b
shift 5; echo "shift 5=$? $#"
shopt -s shift_verbose; shift 5; echo "shift 5 verbose=$? $#"; shopt -u shift_verbose
shift -1; echo "shift -1=$? $#"
shift x; echo "shift x=$? $#"
shift 1 2
echo "shift 1 2=$? $#"
ulimit -z; echo "ulimit -z=$?"
ulimit -n notanumber; echo "ulimit nan=$?"
umask 'u+r,,u-r'; echo "umask ,,=$?"
umask 999; echo "umask 999=$?"
umask 'q+r'; echo "umask q=$?"
umask 'u+rz'; echo "umask z=$?"
umask -x; echo "umask -x=$?"
f() { getopts ab: o -x; echo "getopts x=$? [$o]"; OPTIND=1; getopts ab: o -b; echo "getopts b=$? [$o]"; }; f
. ./missing.sh; echo "dot missing=$?"
. ; echo "dot none=$?"
. nosuch_w12.sh; echo "dot no slash=$?"
. /tmp; echo "dot dir=$?"
foo=1; unset -x foo; echo "unset -x=$? [${foo-unset}]"
unset; echo "unset none=$?"
unset -f; echo "unset -f none=$?"
set -o nosuchopt; echo "set -o=$?"
hash -d nosuchcmd_w; echo "hash -d=$?"
type nosuchcmd_w; echo "type=$?"
command -v nosuchcmd_w; echo "command -v=$?"
read -t x v </dev/null; echo "read -t=$?"
printf '%d\n' abc; echo "printf=$?"
export 'a-b=1'; echo "export=$?"
f2() { local 'a-b=1'; echo "local=$?"; }; f2
declare 'a-b=1'; echo "declare=$?"
readonly 'a-b=1'; echo "readonly=$?"
unset 'a-b'; echo "unset bad=$?"
unset -fv nosuch_w; echo "unset -fv=$?"
yv=Y; declare -n rv=yv; unset -n rv; echo "unset -n=$? [${yv-gone}] [${rv-gone}]"
set +o nosuchopt; echo "set +o=$?"
printf '%d\n' 3abc; echo "printf 3abc=$?"
printf '%x\n' zz; echo "printf x=$?"
declare -Z v; echo "declare -Z=$?"
cd /tmp; rm -rf w12.$$
echo done
