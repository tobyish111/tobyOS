# A declaration builtin's written lists are made as bash expands its
# words, before the builtin assigns anything else: a list's subscript reads
# a value given earlier on the same line as still unset, and a list that
# fails leaves the line's other names unassigned. tsh assigned the words
# in order (finding 615).
exec 2>&1
declare a=1 U=( [a]=x ); declare -p a U
declare c=3 V=( [c+=1]=z ); declare -p c V
f() { local b=2 W=( "$b" [b]=y ); declare -p b W; }; f
declare d=4 X=( [1+]=x )
declare -p d
