# POSIX XCU test(1) -- the `test` / `[` utility.
# `[` is a COMMAND whose last argument must be `]`, not syntax. Every operator
# here is one that shell scripts actually branch on.
t() { if [ "$@" ]; then echo "$1..: true"; else echo "$1..: false"; fi; }

[ -n "x" ]     && echo "1:-n set"
[ -z "" ]      && echo "2:-z empty"
[ -n "" ]      || echo "3:-n empty is false"
[ "a" = "a" ]  && echo "4:string eq"
[ "a" != "b" ] && echo "5:string ne"
[ "a" = "b" ]  || echo "6:string eq false"

[ 1 -eq 1 ]  && echo "7:-eq"
[ 1 -ne 2 ]  && echo "8:-ne"
[ 1 -lt 2 ]  && echo "9:-lt"
[ 2 -le 2 ]  && echo "10:-le"
[ 3 -gt 2 ]  && echo "11:-gt"
[ 3 -ge 3 ]  && echo "12:-ge"
[ 10 -gt 9 ] && echo "13:multi-digit"
[ -5 -lt 0 ] && echo "14:negative"

> afile
[ -e afile ] && echo "15:-e file"
[ -f afile ] && echo "16:-f file"
[ -d /etc ]  && echo "17:-d dir"
[ -d afile ] || echo "18:-d on file false"
[ -f /etc ]  || echo "19:-f on dir false"
[ -e nosuch ] || echo "20:-e missing false"
[ -s afile ] || echo "21:-s empty false"
echo data > bfile
[ -s bfile ] && echo "22:-s nonempty"

[ 1 -eq 1 -a 2 -eq 2 ] && echo "23:-a"
[ 1 -eq 2 -o 2 -eq 2 ] && echo "24:-o"
[ ! 1 -eq 2 ]          && echo "25:!"
[ \( 1 -eq 1 \) ]      && echo "26:parens"

v="spaced value"
[ "$v" = "spaced value" ] && echo "27:quoted var"
e=""
[ -z "$e" ] && echo "28:empty var"

if test 1 -eq 1; then echo "29:test-name"; fi
if [ "x$e" = "x" ]; then echo "30:x-prefix idiom"; fi
echo end
