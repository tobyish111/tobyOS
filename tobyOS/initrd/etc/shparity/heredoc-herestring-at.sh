# A here-string and a here-document body are one string: "$@" and
# "${a[@]}" join with a space there, and the marks tsh records them with
# never reach the reader; a ${x+'b'} in a body keeps its single quotes, as
# in double quotes (finding 301).
set -- a b; x=x; a=(x '' y)
cat <<< "$@" | od -An -c
read -r v <<< "$@"; printf '[%s]\n' "$v"
cat <<< "${a[@]}" | od -An -c
cat <<< ${x+"$@"} | od -An -c
cat <<EOF | od -An -c
<$@> <"$@"> <${a[@]}> <${x+"$@"}>
<${x+"a"}> <${x+'b'}> <${u-"c d"}> <${x+"*"}>
EOF
f() { cat <<EOF
<$@> <${1+"$@"}>
EOF
}
f p q | od -An -c
set --
cat <<< "$@" | od -An -c
cat <<EOF | od -An -c
<$@> <${x+"$@"}>
EOF
