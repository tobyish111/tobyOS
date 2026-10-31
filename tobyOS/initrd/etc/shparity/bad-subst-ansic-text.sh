# "bad substitution" names the text as bash's parser left it: a `$'...'`
# in a pattern word quoted ('t'), and outside double quotes anywhere; in
# double quotes with extquote off, or in a here-document, a name stays as
# written -- tsh named the text as written (finding 479).
exec 2>&1
x=notOK
x1=not
echo "${x#${'x1'%$'t'}}"
echo "${'x1'%$'a\tb'}"
cat <<EOF
${x#${'x1'%$'t'}}
EOF
echo ${x#${$'x1'%$'t'}}
echo [${$'x1'}]
shopt -u extquote
echo "${x#${$'x1'%$'t'}}"
echo after
