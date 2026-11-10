# A readonly indexed array's subscript is judged before the readonly
# refusal: `c[-2]=4` of an empty readonly c is "c[-2]: bad array subscript"
# (finding 390). tsh said "c: readonly variable".
exec 2>&1
declare -r c[100]; c[-2]=4; echo "not here"
echo "rc=$?"
declare -ra d=(1); d[0]=2; echo "not here"
echo "rc=$?"
