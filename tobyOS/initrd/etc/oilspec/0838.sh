# I consider mksh and zsh a bug because \x is not an escape
echo -e '\x' '\xg' | od -A n -c | sed 's/ \+/ /g'
