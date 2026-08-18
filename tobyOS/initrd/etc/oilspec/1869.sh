set -u
shopt -s eval_unsafe_arith || true 2>/dev/null

#preHooks=()
hookSlice="preHooks[@]"

argv.py ${!hookSlice+"${!hookSlice}"}

for element in ${!hookSlice+"${!hookSlice}"}; do
  echo $element
done
