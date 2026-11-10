# Hm this was never a bug, but it's worth testing.
# The bug was EvalToInt() in the condition.

for base in 31 32 62; do

  start=$(( (1 << $base) - 2))
  end=$(( (1 << $base) + 2))

  for ((i = start; i < end; ++i)); do
    echo $i
  done
  echo ---
done
