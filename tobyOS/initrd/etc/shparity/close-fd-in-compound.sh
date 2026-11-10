# `N>&-` at the end of a line inside a compound closes N for that command
# only: the reader took the `-` for an operator still waiting for its
# filename and joined the next line to it, so `: 4>&-` swallowed the
# `echo ... >&4` after it (bash's redir7.sub; finding 300).
exec 4>&1
{
	: 4>&-
	echo "group" >&4
}
while :; do
	true 4>&-
	echo "loop" >&4
	break
done
if true; then
	: 4>&-
	echo "if" >&4
fi
{
	: 5>&-
	echo "unopened" >&4
}
exec 4>&-
exec 4<&0
{
	: 4<&-
	cat <&4 </dev/null; echo "input rc=$?"
}
exec 4<&-
stuff() { c=1; }
exec 3>&1
eval `
exec 4>&1 >&3 3>&-
{
	stuff 4>&-
	echo "c=$c" >&4
}`
echo "c is $c"
