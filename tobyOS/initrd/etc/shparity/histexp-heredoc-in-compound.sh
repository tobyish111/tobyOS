# A here-document's body is never history-expanded, in a command still open
# too -- a { }, an if, a $( ), a function -- where tsh expanded it, and a !!
# there pulled in the whole last command (finding 540).
exec 2>&1
set -o history
set -o histexpand
echo a
{ cat <<EOF
in-group !!
EOF
}
if true; then cat <<EOF
in-if !!
EOF
fi
echo $( cat <<EOF
in-comsub !!
EOF
)
f() { cat <<EOF
in-func !!
EOF
}
f
echo b
echo !!
