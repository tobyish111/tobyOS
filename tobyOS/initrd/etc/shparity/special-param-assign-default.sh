# ${6=word} and ${3:=word} cannot assign a positional parameter: "$6:
# cannot assign in this way", status 1 and the rest of the line dropped
# (finding 401).
exec 2>&1
set -- a; echo ${6="arg6"}; echo "not here"
echo "rc=$?"; echo ${3:=x}
echo "rc=$?"; echo "${1=keep}" "$#"
