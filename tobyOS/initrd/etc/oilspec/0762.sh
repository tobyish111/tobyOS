cd > /dev/null  # for some reason in bash, this makes PIPESTATUS appear!
compgen -v P | grep -E '^PATH|PWD' | sort
