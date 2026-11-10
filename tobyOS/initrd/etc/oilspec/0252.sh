case $SH in bash) ;; *) shopt --set strict_array ;; esac

s1=hello
s2=world

# Overwriting Str with a new BashArray is allowed
eval 's1=(1 2 3 4)'
echo status=$?
declare -p s1
# Promoting Str to a BashArray is disallowed
eval 's2+=(1 2 3 4)'
echo status=$?
declare -p s2
