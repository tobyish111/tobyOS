case $SH in mksh) exit ;; esac

declare undef
unset -v 'undef[1]'
echo undef $?
unset -v 'undef["key"]'
echo undef $?

declare a=(one two)
unset -v 'a[1]'
echo array $?

#shopt -s strict_arith || true
# In OSH, the string 'key' is converted to an integer, which is 0, unless
# strict_arith is on, when it fails.
unset -v 'a["key"]'
echo array $?

declare -A A=(['key']=val)
unset -v 'A[1]'
echo assoc $?
unset -v 'A["key"]'
echo assoc $?
