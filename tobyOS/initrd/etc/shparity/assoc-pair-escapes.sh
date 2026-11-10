# The key/value pair form of an associative list takes the expander's
# escape marks off its words, as the [key]=value form does: `A=(\\ 2)` is
# the key \ -- in a declaration's list, an appended one and a local's too
# (finding 362). tsh stored keys with a 0x04 byte in front.
exec 2>&1
declare -A A; A=(x 1 \\ 2 \; 3 p\"q 4 \{ 5 \} 6 a\ b 7); declare -p A
declare -A B=( \\ bs '\' bs2 \" dq ); declare -p B
declare -A C=(k v); C+=( \\ x \$ y ); declare -p C
f() { local -A L=( \\ 1 \; 2 ); declare -p L; }; f
v=$'\x01'; declare -A D; D=( $v $v$v ); declare -p D
