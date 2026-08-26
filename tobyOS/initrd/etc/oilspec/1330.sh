# bash doesn't abort unless errexit!
readonly foo=bar
foo=eggs
echo "status=$?"  # nothing happens
