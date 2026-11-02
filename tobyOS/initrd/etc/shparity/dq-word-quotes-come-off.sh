# In double quotes and in a here-document, the double quotes of a ${ }
# word come off before it is expanded: `"$"x` is $x, and in a body, where
# no $"..." is translated, `$"def"` is $def. tsh took them as quotes, so
# the body's ${u:-$"def"} was def (finding 564).
exec 2>&1
u=; x=X
cat <<EOF
[${u:-$"def"}] [${zz:-$"d e"}] [${zz:-"$"x}] [${zz:-$"(echo hi)"}]
[${zz:-"a\"b"}] [${zz:-"$(echo "q r")"}]
EOF
echo "[${zz:-"$"x}]" "[${zz:-$"x"}]" "[${zz:-"a b"}]"
