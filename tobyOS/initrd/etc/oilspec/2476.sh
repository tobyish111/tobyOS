# These are runtime errors, but we could make them parse time errors.
v=abcde
echo ${#v:1:3}
# zsh actually implements this!
