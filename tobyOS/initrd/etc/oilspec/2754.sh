set -o xtrace
echo '1 2' \' \" \\

# YSH is different because backslashes require $'\\' and not '\', but that's OK
