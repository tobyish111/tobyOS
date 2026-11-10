for i in 1 2; do
  eval  # zero args
  echo status=$?
  eval echo one
  echo status=$?
  eval 'echo two'
  echo status=$?
  shopt -s simple_eval_builtin
  echo ---
done
