eval 'echo z ${(m)foo} z'
echo status=$?

eval 'echo ${x:-${(m)foo}}'
echo status=$?

# double quoted
eval 'echo "${(m)foo}"'
echo status=$?
