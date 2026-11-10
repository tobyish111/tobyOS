declare -A ref=(['dummy']=v1)
function test-rep {
  echo "==== $1 ===="
  ref['dummy']=$1
  argv.py "${!ref[@]#?}"
  argv.py "${!ref[@]%?}"
  argv.py "${!ref[@]//[a-f]}"
  argv.py "${!ref[@]//[a-f]/x}"
}

v1=value
test-rep v1

v2=
test-rep v2

a1=()
test-rep a1

a2=(element)
test-rep 'a2[0]'

a3=(1 2 3)
test-rep 'a3[@]'
