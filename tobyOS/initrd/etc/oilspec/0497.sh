declare -A alpha=(['1']=2)
echo type=${alpha@a}
shopt -s compat_array
echo type=${alpha@a}
