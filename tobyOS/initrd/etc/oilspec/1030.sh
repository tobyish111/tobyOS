x=3abc
printf '%d\n' $x
echo status=$?
printf '%d\n' xyz
echo status=$?
# zsh should exit 1 in both cases
# fails but also prints 0 instead of 3abc
# osh doesn't print anything invalid
