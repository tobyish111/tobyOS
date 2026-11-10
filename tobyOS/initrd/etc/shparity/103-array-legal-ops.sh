# The other half of 102: the same bytes must still be FINE where they are
# legal, or a rejection fix has quietly broken ordinary shell text.
d=(1 '&' 2)
echo "quoted=${#d[@]} [${d[1]}]"
e=(a 'x;y' 'p|q')
echo "e=${#e[@]} [${e[1]}] [${e[2]}]"
( echo sub ) >/dev/null
echo "subshell=$?"
f=$( echo one; echo two )
echo "cmdsub=[$f]"
g=(one two); echo "still-arrays=${g[1]}"
echo end
