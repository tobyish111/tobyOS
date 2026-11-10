# A command substitution that has prefix assignments of its own must not
# disturb the prefixes of the line around it (S5). tsh kept one prefix table
# for the whole shell: the substitution's run overwrote it, so the outer
# prefixes and the substitution's own result were lost and its inner
# prefixes leaked out.
A=1 B=$(C=2 true; echo x) D="[$A][$B]" env | grep '^D='
A=1 B="$(E=5 F=6 true; echo y)" D="[$A][$B][${E-u}]" env | grep '^D='
P=1 Q=`R=3 true; echo z` S="[$P][$Q]" env | grep '^S='
A=1 B=$(X=1 Y=$(Z=2 true; echo in) true; echo out) D="[$A][$B][${Z-u}]" env | grep '^D='
A=1 B=$(echo "[$A]") env | grep '^B='
k=(zero one); A=${k[1]} B=$(G=7 true; echo w) D="[$A][$B][${G-u}]" env | grep '^D='
echo "after: [${A-u}][${B-u}][${C-u}][${E-u}]"
echo done
