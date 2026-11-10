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
