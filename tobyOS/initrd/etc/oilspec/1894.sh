# https://github.com/oils-for-unix/oils/issues/2269

escape_arg() {
	a="$1"
	until [ -z "$a" ]; do
		case "$a" in
		(\'*) printf "'\"'\"'";;
		(*) printf %.1s "$a";;
		esac
		a="${a#?}"
    echo len=${#a} >&2
	done
}

# encode
phrase="$(escape_arg "that's it!")"
echo escaped "$phrase"

# decode
eval "printf '%s\\n' '$phrase'"

echo ---

# harder input: NUL surrounded with ::
arg="$(printf ':\000:')" 
#echo "arg=$arg"

case $SH in
  zsh) echo 'writes binary data' ;;
  *) echo escaped "$(escape_arg "$arg")" ;;
esac
#echo "arg=$arg"
