# $"..." in the word of a ${...} is a double-quoted string: ${x#$"no	"}
# removes the quoted text -- unquoted, in double quotes and in a
# here-document; tsh read a $ and then a quoted string (finding 449).
exec 2>&1
x=$'no\tOK'; y=notOK
echo "1 ${x#$"no	"}"; echo 2 ${x#$"no	"}; echo "3 ${y#$"not"}"
cat <<EOF
4 ${x#$"no	"}
5 ${y#$"not"}
6 ${x#n$"o	"}
EOF
