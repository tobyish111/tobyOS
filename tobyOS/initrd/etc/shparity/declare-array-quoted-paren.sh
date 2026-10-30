# A name only declared an array takes a quoted (...) value as a list, as an
# array does: `declare -a c; declare c='(one two)'` (finding 330).
{
declare -a c; declare c='(one two three)'; declare -p c
declare -a d=(x); declare d='(p q)'; declare -p d
declare e='(not a list)'; declare -p e
} 2>&1
