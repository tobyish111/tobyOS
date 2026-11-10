# Odd behavior of bash/mksh: $'' is recognized but NOT ''!
x=abc
echo ${x%$'b'*}
echo "${x%$'b'*}"  # git-prompt.sh relies on this
