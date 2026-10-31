# The directory part of a pattern is a path with its quoting removed: an
# expansion's `\/` is a slash and `t\mp` is tmp, so ./tmp${bs}/a/b/* lists
# ./tmp/a/b and shows it so -- tsh opened a directory with the backslash in
# its name (finding 461).
exec 2>&1
d=$(mktemp -d); cd "$d" || exit; mkdir -p tmp/a/b; touch tmp/a/b/c; bs=\\
echo ./tmp${bs}/a/b/*; echo ./tmp${bs}/a/b/c; echo ./t${bs}mp/a/b/*
cd /; rm -rf "$d"
