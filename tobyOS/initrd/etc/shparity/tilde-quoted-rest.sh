# A tilde whose prefix was written unquoted expands though the rest of the
# word is quoted: ~/"foo" is $HOME/foo, while ~"/foo" and "~"/foo stay --
# tsh expanded no tilde in a word quoted anywhere (finding 541).
exec 2>&1
HOME=/usr/xyz
echo ~/"foo"
echo ~/'f o'
echo ~/\*
echo ~/$'x'
echo ~"/foo"
echo "~"/foo
x=~/"foo"; echo "$x"
echo a=~/"foo" b=~:"c"
for w in ~/"in"; do echo "$w"; done
HOME=$PWD
echo hi > ~/"r.txt"
cat r.txt
