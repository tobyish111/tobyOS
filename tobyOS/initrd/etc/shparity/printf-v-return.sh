# printf's options as bash's internal_getopt "v:" reads them: -v's name
# checked as it is read, attached or not, `--`, a bad letter the usage; and
# return's order: its number first, then "too many arguments" -- which
# drops the rest of the line with 1 -- then the function test. `[`'s
# missing `]' in bash's quotes.
p() { echo "== printf $*"; ( printf "$@" ) 2>&1; echo "rc=$?"; }
p -v
p -v x
p -v x ''
p -v 1x %s a
p -v 'a[' %s a
p -v 'a[1]' %s a
p -vx %s a
p -x %s
p -- -x
p -5
p --
p -v --help
p -v x -- %s y
v=old; printf -v v '%s-%s' a b; echo "v=$v"
printf -vw '[%s]' c; echo "w=$w"
r() { echo "== $1"; ( eval "$1" ) 2>&1; echo "rc=$?"; }
r 'f() { return 3 4; echo in; }; f; echo after'
r 'f() { return 3 4; }; f
echo next-line'
r 'f() { return abc; }; f; echo "after=$?"'
r 'f() { return abc def; }; f; echo "after=$?"'
r 'return 3 4; echo top'
r 'return abc; echo "top=$?"'
r 'f() { return -- 7; }; f; echo "after=$?"'
r 'f() { return 300; }; f; echo "after=$?"'
r '[ a'
r '[ a = a'
r '[ a = a ]; echo ok'
