# A PREFIX ASSIGNMENT IS IN FORCE FOR THE WORDS AFTER IT, and not for the
# ones before it. The bindings are made left to right, so BAZ is empty in
# BAR's value while FOO is not.
FOO=foo BAR="[$FOO][$BAZ]" BAZ=baz sh -c 'echo "$BAR"'
echo "after=[$FOO][$BAR][$BAZ]"
A=1 B=$A sh -c 'echo B=$B'
echo done
