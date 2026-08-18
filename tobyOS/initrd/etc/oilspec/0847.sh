set -e

sh_func_that_evals() {
  local code_str=$1
  for i in 1 2; do
    echo $i
    eval "$code_str"
  done
  echo 'end func'
}

for code_str in break continue return false; do
  echo "--- $code_str"
  sh_func_that_evals "$code_str"
done
echo status=$?
