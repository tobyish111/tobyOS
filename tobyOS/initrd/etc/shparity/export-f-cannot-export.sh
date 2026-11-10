# A function whose name has `=` or `/` in it cannot be exported: "export:
# NAME: cannot export", status 1 -- tsh exported it, and a child ran the
# function for /bin/echo (finding 511).
exec 2>&1
function foo=bar
{
	echo eq
}
export -f 'foo=bar'; echo "status: $?"
function /bin/echo
{
	echo bad echo
}
export -f /bin/echo; echo "status: $?"
"$BASH" -c '/bin/echo child-ok'
env | grep -c 'BASH_FUNC_foo'
