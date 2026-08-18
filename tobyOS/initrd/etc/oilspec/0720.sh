shopt -s strict_arg_parse

mkdir -p foo
cd foo
echo status=$?
cd ..
echo status=$?


cd foo bar
st=$?
if test $st -ne 0; then
  echo 'failed with multiple args'
fi
