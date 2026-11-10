# A bracket expression cannot hold a bare `/` in a pathname: `[qwe/qwe]`
# is a name, not a pattern, and nullglob keeps it -- tsh dropped it
# (finding 460).
exec 2>&1
shopt -s nullglob
echo 1: [qwe/qwe]; echo 2: [qwe/; echo 3: [qwe/]; echo 5: [qwe\/
shopt -u nullglob
echo 7: [ab/c]
