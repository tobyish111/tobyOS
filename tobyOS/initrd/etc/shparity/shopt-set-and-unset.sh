# shopt -s and -u together are refused: "shopt: cannot set and unset shell
# options simultaneously", status 1, the option left as it was (finding 419).
exec 2>&1
shopt -s checkhash; shopt -s -u checkhash; echo "rc=$?"; shopt -u -s checkhash; echo "rc=$?"
shopt -su checkhash; echo "rc=$?"; shopt checkhash
