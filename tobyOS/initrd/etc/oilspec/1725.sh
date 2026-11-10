shopt -s globstar

mkdir -p c/subdir
touch {leaf.md,c/leaf.md,c/subdir/leaf.md}

echo **/*.* | tr ' ' '\n' | sort
echo
echo **/**/*.* | tr ' ' '\n' | sort
