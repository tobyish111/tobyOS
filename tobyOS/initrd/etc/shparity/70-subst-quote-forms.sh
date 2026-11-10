# The six forms virtualenv's bin/activate depends on. Double quotes inside
# backticks are NOT the same as double quotes inside $(), and all shells
# agree on that -- so the two paths have to be kept apart.
echo "x $(echo hi)"
echo "x $(echo "hi")"
echo "x $(echo \"hi\")"
echo "x `echo hi`"
echo "x `echo "hi"`"
echo "x `echo \"hi\"`"
