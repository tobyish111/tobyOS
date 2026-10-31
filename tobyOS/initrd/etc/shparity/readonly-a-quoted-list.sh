# With -a or -A, readonly takes a quoted list as a list: `readonly -a
# 'd=(4)'` is the array d=(4), and refused for a readonly name it is
# readonly's refusal, "readonly: d: readonly variable" -- tsh set the
# string "(4)" (finding 489). Without -a it stays a string.
exec 2>&1
f() { readonly -a 'd=(4)'; }; f; declare -p d
g() { readonly -a e='(5 6)'; }; g; declare -p e
h() { readonly 'c=(3)'; }; h; declare -p c
readonly -a 'd=(7)'; echo "rc=$?"
readonly -a d='(8)'; echo "rc=$?"
s=1; readonly s
readonly 's=2'; echo "rc=$?"
readonly -a 's=(3)'; echo "rc=$?"
