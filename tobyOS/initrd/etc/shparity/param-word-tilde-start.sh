# The word of ${x:=WORD} as a command's argument tilde-expands at its start
# only; as an assignment's value, after each `:` too -- tsh expanded after
# the colons everywhere (finding 498).
exec 2>&1
HOME=/usr/xyz
X=/bin:.
unset A; : ${A:=~/bin:~/bin2:$X}; echo $A
unset A; : ${A:=PATH=~/bin:~/bin2:$X}; echo $A
unset A; B=${A:-~/b:~/c}; echo $B
echo ${A:-~/d:~/e}
