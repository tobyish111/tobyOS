# A SUBSTITUTION HAS ITS OWN QUOTE STATE, and the line reader has to agree.
# An apostrophe inside a nested double-quoted string used to read as an
# opening single quote, so the reader judged the line incomplete and glued
# every following line onto it as extra arguments.
echo "[$(printf "that's")]"
echo A

p="$(printf "that's")"
echo "got [$p]"
echo B

esc() {
	printf X
}
q="$(esc "that's it!")"
echo "got [$q]"
echo C

# the same shape one level deeper, where the paren matcher had to learn it
# as well: this printed nothing at all
echo "[$(echo "$(printf "it's")")]"
echo D

# an unterminated string inside a substitution IS still unterminated, so this
# line continues onto the next
echo "[$(printf "un
terminated")]"
echo E

# the real thing: strip one character at a time, apostrophe and all
escape_arg() {
	a="$1"
	until [ -z "$a" ]; do
		case "$a" in
		(\'*) printf "'\"'\"'";;
		(*) printf %.1s "$a";;
		esac
		a="${a#?}"
	done
}
phrase="$(escape_arg "that's it!")"
echo escaped "$phrase"
eval "printf '%s\n' '$phrase'"
echo F
