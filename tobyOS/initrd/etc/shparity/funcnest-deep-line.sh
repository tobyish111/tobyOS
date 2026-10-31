# A command's line in a function called more than two dozen deep is still
# its own line: FUNCNEST's message at depth 60 names the call -- tsh named
# the body's first line once the line-map stack ran out (finding 506).
exec 2>&1
FUNCNEST=60
g()
{
	(( n++ ))
	if (( n > 200 )); then
		return 7
	fi
	g
}
n=0; g; echo "rc=$? n=$n"
echo "next $n"
