printf '%s\n' 'a b\' 'c d' | (read; argv.py "$REPLY")
printf '%s\n' 'a b\,c d'   | (read; argv.py "$REPLY")
printf '%s\n' 'a b\' 'c d' | (read -d ,; argv.py "$REPLY")
printf '%s\n' 'a b\,c d'   | (read -d ,; argv.py "$REPLY")

# mksh/zsh swallows "backslash + delim" instead.
['a bc d']
['a b,c d']
['a b\nc d']
['a bc d']
