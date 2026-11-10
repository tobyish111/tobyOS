# This isn't specified by the POSIX grammar, but it's accepted by both dash and
# bash!
echo foo > foo.txt
echo bar > bar.txt
cat <<EOF 1>&2 foo.txt - bar.txt
here
EOF
