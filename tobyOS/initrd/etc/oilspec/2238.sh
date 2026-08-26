shopt -s ignore_shopt_not_impl
for name in foo autocd cdable_vars checkwinsize; do
  shopt -s $name
  echo $?
done
