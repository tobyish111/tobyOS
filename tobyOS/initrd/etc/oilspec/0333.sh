$SH -c 'shopt -s strict_array; s="abc"; echo ${s[@]}'
echo status=$?

$SH -c 'shopt -s strict_array; s="abc"; echo ${s[*]}'
echo status=$?
