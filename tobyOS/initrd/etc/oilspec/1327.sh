old=$PATH
export PATH
new=$PATH
test "$old" = "$new" && echo "not changed"
