# POSIX 2.7.1/2.7.2 -- input and output redirection, including truncation vs
# append and the fact that a redirection applies to the command it is attached
# to, wherever it appears in the word list.
echo first > out.txt
while read l; do echo "a:$l"; done < out.txt

echo second >> out.txt
while read l; do echo "b:$l"; done < out.txt

echo third > out.txt
while read l; do echo "c:$l"; done < out.txt

> empty.txt
n=0
while read l; do n=$((n+1)); done < empty.txt
echo "empty-lines=$n"

> pre.txt echo leading-redirect
while read l; do echo "d:$l"; done < pre.txt

echo mid > mid.txt
while read l; do echo "e:$l"; done < mid.txt

{ echo g1; echo g2; } > grp.txt
while read l; do echo "f:$l"; done < grp.txt

printf 'p1\np2\n' > pf.txt
while read l; do echo "g:$l"; done < pf.txt
echo end
