# A tilde in a here-document's ${ } word is not expanded: bash leaves
# ${zz:-~} there as ~. tsh gave the home directory (finding 565).
exec 2>&1
HOME=/H
cat <<EOF
[${zz:-~}] [${zz:-~/a}] [${zz:-"~"}]
EOF
echo ${zz:-~} "${zz:-~}"
