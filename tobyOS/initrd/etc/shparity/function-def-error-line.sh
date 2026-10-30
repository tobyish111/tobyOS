# A function definition runs on the line it ends on, so the refusal to
# redefine a readonly function names that line, not its first (finding 414).
exec 2>&1
func() { echo f; }
readonly -f func
func()
{
	echo bar
}
echo "rc=$?"
func
