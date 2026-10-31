# Inside double quotes a parameter name written `$'...'` is its decoded
# text when extquote is on, and so it is in a pattern word nested there or
# in a here-document: "${x#${$'x1'%$'t'}}" is tOK -- tsh said "bad
# substitution" (finding 478).
exec 2>&1
x=notOK
x1=not
echo "${x#${$'x1'%$'t'}}"
echo "${x#${$'x1'%'t'}}"
echo "[${$'x1'}] [${#$'x1'}]"
cat <<EOF
${x#${$'x1'%$'t'}}
${x#${$'x1'%'t'}}
EOF
echo after
