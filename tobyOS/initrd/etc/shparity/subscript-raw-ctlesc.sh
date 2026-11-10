# A raw ^A in an assignment's subscript is data: W[^A]=v keys ^A, and
# a[1^A]=v is an invalid arithmetic operator. tsh read it as a field mark:
# the key "", and element 1 (finding 570).
exec 2>&1
declare -A W
eval "W[$(printf '\001')]=v; W[$(printf '\001\001')]=w"; declare -p W
eval "a[1$(printf '\001')]=bar"
echo "rc=$?"; declare -p a
echo end
