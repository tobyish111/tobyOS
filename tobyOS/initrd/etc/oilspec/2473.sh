case $SH in zsh) exit ;; esac

HOST_PATH=/foo/bar/baz
echo ${HOST_PATH////\\/}

# The way bash parses it
echo ${HOST_PATH//'/'/\\/}


# zsh has crazy bugs
