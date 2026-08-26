declare -A ref=(['dummy']=v1)
function test-op0 {
  echo "==== $1 ===="
  ref['dummy']=$1
  argv.py "${!ref[@]@Q}"
  argv.py "${!ref[@]@P}"
  argv.py "${!ref[@]@a}"
}

v1=value
test-op0 v1

v2=
test-op0 v2

a1=()
test-op0 a1

a2=(element)
test-op0 'a2[0]'

a3=(1 2 3)
test-op0 'a3[@]'


# Bash 4.4 had a bug in the section "==== a3[@] ====":
#
# ==== a3[@] ====
# []
# []
# []
