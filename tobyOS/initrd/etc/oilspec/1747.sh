# read can't be run in a subshell.
read v1 v2 <<EOF
val1 val2
EOF
echo =$v1= =$v2=
