shopt -s globstar

mkdir -p c/subdir
touch c/subdir/leaf.md

echo {**/*.*,} | sort | sed 's/[[:space:]]*$//'
