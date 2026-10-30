# A bare word in an associative literal that has [key]=value elements is
# skipped with bash's warning, "chaff: four: must use subscript when
# assigning associative array" (finding 353). tsh skipped it silently.
{
declare -A chaff; declare -i chaff; chaff=( [zero]=1+4 [one]=3+7 four ); echo "rc=$?"; declare -p chaff
declare -A all=(1 2 3); declare -p all
} 2>&1
