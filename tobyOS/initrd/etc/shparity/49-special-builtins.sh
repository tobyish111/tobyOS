# POSIX 2.14 -- the special built-in utilities.
# `:` is the null command, `.` sources in the CURRENT environment, `eval`
# re-parses its arguments, `exec` without a command applies redirections to
# the shell itself, and `shift`/`unset`/`readonly` do what their names say.
:
echo "1=$?"
: ignored args
echo "2=$?"

eval 'echo "3:eval-simple"'
eval "echo 4:eval-double"
x=inner
eval 'echo "5:$x"'
cmd='echo 6:from-variable'
eval "$cmd"
eval 'a=7; echo "7:$a"'
echo "8:$a"

printf '%s\n' 'echo 9:sourced' 'sv=sourced-var' > lib.sh
. ./lib.sh
echo "10:$sv"

set -- p q r s
shift
echo "11:[$1] n=$#"
shift 2
echo "12:[$1] n=$#"

u=defined
unset u
echo "13=[${u-unset-now}]"

ro=fixed
readonly ro
echo "14=$ro"

export EV=exported
echo "15=$EV"

f() { return 16; }
f
echo "16=$?"

( exec > ex.txt; echo "17:via-exec" )
while read l; do echo "18:$l"; done < ex.txt

true
echo "19=$?"
false
echo "20=$?"
echo end
