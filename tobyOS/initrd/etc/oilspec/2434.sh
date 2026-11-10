empty=''
x=x

echo ${x@Q} ${empty@Q} ${undef@Q} ${x@Q}

echo ${x@K} ${empty@K} ${undef@K} ${x@K}

echo ${x@k} ${empty@k} ${undef@k} ${x@k}

echo ${x@A} ${empty@A} ${undef@A} ${x@A}

declare -r x
echo ${x@a} ${empty@a} ${undef@a} ${x@a}

# x x
#echo ${x@E} ${empty@E} ${undef@E} ${x@E}
# x x
#echo ${x@P} ${empty@P} ${undef@P} ${x@P}
