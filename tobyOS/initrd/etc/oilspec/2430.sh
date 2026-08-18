# note: "y z" causes a bug!
$SH -c 'declare -A A=(["x"]="y"); echo ${A@P} - ${A[@]@P}'
echo status=$?

# note: "y z" causes a bug!
$SH -c 'declare -A A=(["x"]="y"); echo ${A@Q} - ${A[@]@Q}' | sed 's/^- y$/- '\''y'\''/'
echo status=$?

$SH -c 'declare -A A=(["x"]=y); echo ${A@a} - ${A[@]@a}'
echo status=$?
