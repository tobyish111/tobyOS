# bash is the only one that does it first.  I guess since this is
# non-POSIX anyway, follow bash?
i=0
echo {a,b,c}-$((i++))
