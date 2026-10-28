# An array literal's words are words, reserved or not: bash's `foo=(for
# case if then else)` has five elements. tsh's scanner opened a `for`
# there, and the list splitter, which lacked the text's start, read `=(`
# as a subshell: the line ran unsplit, its echo expanded before the
# assignment (bash's array.tests; finding 291).
foo=(for case if then else); echo "${foo[@]}"
foo=(for x); echo "n=${#foo[@]}"; echo end
foo=(a for b do done c); echo ${#foo[@]} "${foo[@]}"
declare -a g=(while until select function time); echo "${g[@]}"
f() { local -a l=(fi esac done); echo "${l[@]}"; }; f
x=(for
case in
esac); echo "${x[@]}"
if true; then a=(if b); echo "${a[@]}"; fi
