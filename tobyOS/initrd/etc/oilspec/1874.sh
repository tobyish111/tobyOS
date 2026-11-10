shopt -s nocasematch
[[ a =~ A ]]; echo $?
[[ A =~ a ]]; echo $?
[[ a =~ [A] ]]; echo $?
[[ A =~ [a] ]]; echo $?
