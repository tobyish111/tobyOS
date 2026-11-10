case $SH in bash) ;; *) shopt --set strict_array ;; esac

s1=hello
s2=world

# Overwriting Str with a new BashAssoc is allowed
eval 'declare -A s1=([a]=x [b]=y)'
echo status=$?
declare -p s1
# Promoting Str to a BashAssoc is disallowed
eval 'declare -A s2+=([a]=x [b]=y)'
echo status=$?
declare -p s2
