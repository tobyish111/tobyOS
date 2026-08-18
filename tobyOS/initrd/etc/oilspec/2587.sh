declare -A ref=(['dummy']=v1)
function test-suffixes {
  echo "==== $1 ===="
  ref['dummy']=$1
  argv.py "${!ref[@]:2}"
  argv.py "${!ref[@]:1:2}"
  argv.py "${!ref[@]:-empty}"
  argv.py "${!ref[@]:+set}"
  argv.py "${!ref[@]:=assign}"
}

v1=value
test-suffixes v1
echo "v1=$v1"

v2=
test-suffixes v2
echo "v2=$v2"

a1=()
test-suffixes a1
argv.py "${a1[@]}"

a2=(element)
test-suffixes 'a2[0]'
argv.py "${a2[@]}"

a3=(1 2 3)
test-suffixes 'a3[@]'
argv.py "${a3[@]}"
