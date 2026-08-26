cd > /dev/null  # for some reason in bash, this makes PIPESTATUS appear!
compgen -e P | grep -E '^PATH|PWD' | sort
