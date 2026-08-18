declare -i foo=2+3
echo status=$?
echo foo=$foo
echo

shopt -s ignore_flags_not_impl
declare -i bar=2+3
echo status=$?
echo bar=$bar


# bash doesn't need this
