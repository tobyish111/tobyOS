exec 2>&1 </dev/null
printf 'ab\n' | cat
echo ab | cat
printf 'ab\n' | /bin/tsh -c 'read x; echo "len=${#x}"'
/bin/echo ab | /bin/tsh -c 'read x; echo "len=${#x}"'
/bin/echo ab | /bin/tsh -c 'cat'
/bin/echo ab | /bin/tsh -c 'x=$(cat); echo "[$x]"'
/bin/echo ab | /bin/bash -c 'read x; echo "len=${#x}"'
/bin/echo ab > /tmp/q57.in; /bin/tsh -c 'read x; echo "len=${#x}"' < /tmp/q57.in
/bin/echo ab | /bin/tsh -c 'read -r x; echo "rc=$? len=${#x}"'
/bin/echo ab | /bin/tsh /dev/stdin
echo end
