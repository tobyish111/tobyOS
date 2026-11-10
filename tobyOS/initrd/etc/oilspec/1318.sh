foo=old
export -n foo=new
echo status=$?
echo $foo
