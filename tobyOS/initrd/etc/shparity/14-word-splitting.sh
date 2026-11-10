# Word splitting and IFS. Unquoted expansion splits, quoted expansion does
# not, and IFS decides where the splits fall. This is the rule that makes
# unquoted "$var" a bug rather than a style choice -- so a superset shell has
# to reproduce it exactly, including the surprising parts.
v="a b c"
for w in $v;   do echo "split:$w";  done
for w in "$v"; do echo "quoted:$w"; done

s="  leading and   internal  "
for w in $s; do echo "trim:[$w]"; done

IFS=:
p="x:y:z"
for w in $p; do echo "ifs:$w"; done
p2="a::b"
for w in $p2; do echo "empty-field:[$w]"; done

IFS=" "
back="p q"
for w in $back; do echo "restored:$w"; done
echo splitting-done
