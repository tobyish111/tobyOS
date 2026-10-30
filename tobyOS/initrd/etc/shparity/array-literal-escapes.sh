# An array literal's [key]=value element loses the expander's escape marks,
# as an assignment's value does: `foo=([bar\"bie]=doll)` is the key
# bar"bie, and `a=([0]=bar\"bie)` the value (finding 351). tsh stored a
# 0x04 byte before the escaped character, in keys and in values.
{
declare -A foo; foo=([bar\"bie]=doll); echo ${!foo[@]}; declare -p foo
declare -A g; g=([bar\'bie]=doll); declare -p g; declare -A h; h=([bar\$bie]=doll); declare -p h
a=([0]=bar\"bie [1]="x\"y"); declare -p a
declare -A A; A=([a\b]=c\d [t]=\~ [u]=~); declare -p A; b=([0]=x\y [1]=\~); declare -p b
} 2>&1
