case $SH in zsh) exit ;; esac

shopt -u globstar

mkdir -p c/subdir
touch {leaf.md,c/leaf.md,c/subdir/leaf.md}

echo **/*.* | sort
