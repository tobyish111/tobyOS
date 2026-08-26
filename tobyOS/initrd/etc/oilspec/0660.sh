bind -v | grep blink-matching-paren
echo

# transform silly quote so we don't mess up syntax highlighting
bind -V | grep blink-matching-paren | sed "s/\`/'/g"
