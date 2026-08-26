# GAP 1: field splitting of unquoted expansions.
# `echo` hides this bug -- it joins its arguments with spaces either way -- so
# every check here counts ARGUMENTS ($#) rather than looking at printed text.
count() { echo "n=$#"; }

v="a b c"
count $v
count "$v"
for w in $v; do echo "for:$w"; done
for w in "$v"; do echo "forq:$w"; done

s="  leading   internal  trailing  "
count $s
for w in $s; do echo "trim:[$w]"; done

empty=""
count $empty
count "$empty"

IFS=:
p="1:2:3"
count $p
for w in $p; do echo "ifs:$w"; done
p2="a::b"
for w in $p2; do echo "field:[$w]"; done

IFS=" "
back="p q"
count $back
echo splitting-done
