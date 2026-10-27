# $"..." inside [[ ]] is "..." (the C locale translates nothing), as it is
# outside (batch 37: logs/b37/quotemix.sh). Handed on with its `$`, every
# comparison came out false.
x=1
[[ $"ab" == ab ]] && echo yes1 || echo no1
[[ $"a'b" == "a'b" ]] && echo yes2 || echo no2
[[ $"a b" == "a b" ]] && echo yes3 || echo no3
[[ "a'b" == $"a'b" ]] && echo yes4 || echo no4
[[ $"a*" == $"a*" ]] && echo yes5 || echo no5
[[ ab == $"a"* ]] && echo yes6 || echo no6
[[ az == $"a*" ]] && echo yes7 || echo no7
[[ -n $"x" ]] && echo yes8 || echo no8
[[ $"$x" == 1 ]] && echo yes9 || echo no9
[[ $"a" == a && $x == 1 ]] && echo yes10 || echo no10
[[ $'a\'b' == "a'b" ]] && echo yes11 || echo no11
