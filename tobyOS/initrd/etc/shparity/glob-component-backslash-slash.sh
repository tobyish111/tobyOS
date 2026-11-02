# A backslash before a slash goes from a path component that is no
# pattern, `./tmp\/a/b/*` finding ./tmp/a/b/c, and stays in one that is:
# `./tm[p]\/a/b/c` matches nothing. tsh dropped it from both
# (finding 581).
exec 2>&1
d=$(mktemp -d); cd "$d" || exit 1
mkdir -p tmp/a/b; touch tmp/a/b/c; bs=\\
echo ./tmp${bs}/a/b/* ./tmp${bs}/a/b/c
echo ./tm[p]${bs}/a/b/c ./t?p${bs}/a/b/*
echo ./tm[p]\/a/b/c
cd /; rm -rf "$d"
