PS1='foo \# bar'
prev_cmd_num="$(echo "${PS1@P}" | egrep -o 'foo [0-9]+ bar' | sed -E 's/foo ([0-9]+) bar/\1/')"
echo "${PS1@P}" | egrep -q "foo $((prev_cmd_num + 1)) bar"
echo matched=$?
