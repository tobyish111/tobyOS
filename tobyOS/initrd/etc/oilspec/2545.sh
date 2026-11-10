set -- ""
echo argv=${@-minus}
echo argv=${@+plus}
echo argv=${@:-minus}
echo argv=${@:+plus}

# Zsh treats $@ as an array unlike Bash converting it to a string by joining it
# with a space.
