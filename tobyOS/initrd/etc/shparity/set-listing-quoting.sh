# The set listing quotes a value only when it holds a shell metacharacter --
# a # or ~ counts only where a word would start -- and a lone ' is \'; tsh
# quoted anything outside a set of plain characters (finding 434).
exec 2>&1
SQ="'"; VT='~'; VH=ab#cd; VH2=#abcd; VS='a b'; VD='$x'; VE=; VQ="a'b"; VB='a\b'
VG='a*'; VC='a=b'; VP='a|b'; VT2='a~'; VT3='a=~'; VT4='a:~'; VBR='{a}'; VEX='!x'; VCA='a^b'; VPC='50%'
set | grep -E '^(SQ|VT|VH|VH2|VS|VD|VE|VQ|VB|VG|VC|VP|VT2|VT3|VT4|VBR|VEX|VCA|VPC)='
