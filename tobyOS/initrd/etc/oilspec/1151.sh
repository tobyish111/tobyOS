set a b
echo "$@"

set - a b
echo "$@"

set -- a b
echo "$@"

set - -
echo "$@"

set -- --
echo "$@"

# note: zsh is different, and yash is totally different
