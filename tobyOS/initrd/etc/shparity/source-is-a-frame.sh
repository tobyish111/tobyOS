# `.` is a frame of the call stack: FUNCNAME "source", BASH_SOURCE the
# file, BASH_LINENO the line of the `.`; a function's BASH_SOURCE is the
# file it was defined in; FUNCNAME stays unset at a sourced file's top, and
# the RETURN trap runs once the frame is gone -- tsh pushed nothing
# (finding 528).
exec 2>&1
cat > s1.sub <<'EOF'
echo "top: [${FUNCNAME[*]-unset}] ${BASH_SOURCE[*]##*/} | ${BASH_LINENO[*]}"
sf() { echo "sf: ${FUNCNAME[*]} | ${BASH_SOURCE[*]##*/} | ${BASH_LINENO[*]}"; }
sf
EOF
. ./s1.sub
f() { source ./s1.sub; }
f
sf
echo '. ./s1.sub' > s2.sub
. ./s2.sub
trap 'echo "R ${BASH_SOURCE[*]##*/} ${BASH_LINENO[*]}"' RETURN
. ./s1.sub
trap - RETURN
echo "after: ${#BASH_SOURCE[@]} ${BASH_LINENO[*]}"
