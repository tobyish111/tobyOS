shopt -s strict_nameref

ref='#'
echo ref=$ref
typeset -n ref
echo ref=$ref
