typeset -a array
array=('' nonempty)

# This feels inconsistent with the rest of bash?
zero=0

[[ -v array[zero+0] ]]
echo zero=$?

[[ -v array[zero+1] ]]
echo one=$?

[[ -v array[zero+2] ]]
echo two=$?

echo ---

i='0+0'
[[ -v array[i] ]]
echo zero=$?

i='0+1'
[[ -v array[i] ]]
echo one=$?

i='0+2'
[[ -v array[i] ]]
echo two=$?

echo ---

i='0+0'
[[ -v array[$i] ]]
echo zero=$?

i='0+1'
[[ -v array[$i] ]]
echo one=$?

i='0+2'
[[ -v array[$i] ]]
echo two=$?
