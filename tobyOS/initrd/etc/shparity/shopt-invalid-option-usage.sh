# An option letter shopt does not have is "shopt: -z: invalid option" and
# bash's usage line, status 2 -- tsh left the usage line out (finding 430).
exec 2>&1
shopt -z; echo "rc=$?"; shopt -sz extglob; echo "rc=$?"
