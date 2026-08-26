# POSIX 2.6.5 -- field splitting.
# The rule with teeth: IFS whitespace collapses and is stripped at the ends,
# but each non-whitespace IFS character is a delimiter in its own right, so
# adjacent ones produce empty fields.
count() { echo "n=$#"; }
show() { echo "n=$# [$1][$2][$3][$4]"; }

v="a b c"
count $v
show $v
count "$v"

w="  pad   middle   pad  "
count $w
show $w

IFS=:
p="1:2:3"
count $p
show $p
q="a::b"
count $q
show $q
r=":lead"
count $r
show $r
t="trail:"
count $t
show $t

IFS=":,"
m="a,b:c"
count $m
show $m

IFS=" :"
mix="a b:c"
count $mix
show $mix

IFS=" "
back="x y"
count $back
empty=""
count $empty
unset IFS
count $v
echo end
