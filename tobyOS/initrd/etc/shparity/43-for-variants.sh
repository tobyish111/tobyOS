# POSIX 2.9.4.2 -- the for loop.
# `for x` with no `in` iterates the POSITIONAL PARAMETERS, which is the form
# used inside functions to walk arguments and is easy to leave unimplemented.
for a in x y z; do echo "1:$a"; done
for a in; do echo "2:$a"; done
echo "3=$?"

set -- p q r
for a; do echo "4:$a"; done
for a in "$@"; do echo "5:$a"; done
for a in "$*"; do echo "6:$a"; done

set -- "one two" three
for a; do echo "7:[$a]"; done

for a in 1 2 3; do
    for b in x y; do
        echo "8:$a$b"
    done
done

n=0
for a in a b c d; do n=$((n+1)); done
echo "9=$n"

for a in $(echo s1 s2); do echo "10:$a"; done
list="l1 l2"
for a in $list; do echo "11:$a"; done
for a in "$list"; do echo "12:$a"; done

for a in single; do echo "13:$a"; done
echo "14-after=$a"
for i in 1 2 3; do echo "15:$i"; done > forout.txt
while read l; do echo "16:$l"; done < forout.txt
echo end
