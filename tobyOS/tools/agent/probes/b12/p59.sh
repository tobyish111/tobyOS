exec 2>&1
mkdir -p /tmp/p59d
mapfile L < /tmp/p59d; echo "mapfile $? ${#L[@]}"
read x < /tmp/p59d; echo "read $? [$x]"
read -r -n 3 x < /tmp/p59d; echo "readn $? [$x]"
read -d '' x < /tmp/p59d; echo "readd $? [$x]"
x=$(< /tmp/p59d); echo "dollar-lt $? [$x]"
cat < /tmp/p59d; echo "cat $?"
while read -r l; do echo "l=$l"; done < /tmp/p59d; echo "while $?"
{ echo grp; } < /tmp/p59d; echo "group $?"
echo hi > /tmp/p59d; echo "gt $?"
echo hi >> /tmp/p59d; echo "gtgt $?"
echo hi 1<> /tmp/p59d; echo "rw $?"
source /tmp/p59d; echo "source $?"
. /tmp/p59d; echo "dot $?"
exec 3< /tmp/p59d; echo "exec3 $?"
read -u 3 y; echo "read-u $? [$y]"
exec 3<&-
rmdir /tmp/p59d
