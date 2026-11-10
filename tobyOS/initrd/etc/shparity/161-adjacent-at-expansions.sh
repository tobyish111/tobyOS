# TWO "@" EXPANSIONS IN ONE WORD JOIN AT THE SEAM (S3): each expansion's
# elements are separate words, but the last element of one and the first of
# the next are one word, with any text between them. tsh ended the word at
# every expansion: "${a[@]}${b[@]}" gave [1][2][x][y] where bash gives
# [1][2x][y].
a=(1 2); b=(x y); c=(); d=(only); e=(""); set -- A B
printf '[%s]' "${a[@]}${b[@]}"; echo " a"
printf '[%s]' "p${a[@]}m${b[@]}s"; echo " b"
printf '[%s]' "$@$@"; echo " c"
printf '[%s]' "${a[@]}-$@"; echo " d"
printf '[%s]' "${a[@]/1/one}${b[@]^}"; echo " e"
printf '[%s]' "${!a[@]}${a[@]}"; echo " f"
printf '[%s]' "${c[@]}${a[@]}"; echo " g"
printf '[%s]' "${a[@]}${c[@]}"; echo " h"
printf '[%s]' "${c[@]}${c[@]}"; echo " i"
printf '[%s]' "x${c[@]}${c[@]}y"; echo " j"
printf '[%s]' "${d[@]}${a[@]}${d[@]}"; echo " k"
printf '[%s]' "${e[@]}${a[@]}"; echo " l"
printf '[%s]' "${a[@]}${e[@]}"; echo " m"
printf '[%s]' "${a[@]:1}${b[@]:0:1}"; echo " n"
printf '[%s]' $@$@; echo " o"
printf '[%s]' "$*$*"; echo " p"
printf '[%s]' "${a[*]}${b[@]}"; echo " q"
x="${a[@]}${b[@]}"; printf '[%s]' "$x"; echo " r"
set --; printf '[%s]' "$@$@"; echo " s"
set -- ""; printf '[%s]' "$@$@"; echo " t"
arr=("${a[@]}${b[@]}"); echo "u ${#arr[@]} ${arr[1]}"
for w in "${a[@]}${b[@]}"; do printf '<%s>' "$w"; done; echo " v"
echo done
