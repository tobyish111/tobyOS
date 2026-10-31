# Under extdebug declare -F gives the line a function's NAME is on -- tsh
# gave its body's first line, 0 for one eval defined -- and 0 for one
# imported (finding 538).
exec 2>&1
shopt -s extdebug
a()
{
    :
}
function b
{
    :
}
:
eval 'e() { :; }'
declare -F a b e
g() { :; }
export -f g
"$BASH" -c 'shopt -s extdebug; declare -F g'
