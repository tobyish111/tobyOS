# An escaped or quoted IFS character in a word or an array literal is data,
# not a delimiter: `IFS=: a=(x\:y)` is one element. tsh split it.
# (batch 12; probe p3 in C:\t\b12)
exec 2>&1
IFS=:
a=(x\:y); echo "a1 ${#a[@]} ${a[0]}"
a=(x:y); echo "a2 ${#a[@]} ${a[0]}"
set -- ${u:-p\:q}; echo "d1 $#"
set -- ${u:-p:q}; echo "d2 $#"
IFS=' '
set -- ${u:-a\ b}; echo "d3 $# $1"
a=(ham\ cheese); echo "a3 ${#a[@]} ${a[0]}"
x=\~/foo; echo "t1 $x"
a=(\~ \*); echo "t2 ${a[*]}"
echo \~ \* a\ b "q\ r"
set -- ${u:-\~}; echo "t3 $1"
case x\:y in x:y) echo c1;; esac
v=a\ b; echo "v $v"
echo ${u:-$'p\tq'} ${u:-"r s"}
