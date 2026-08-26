shopt -s expand_aliases
alias c='cat <<EOF
$(echo hi)
EOF
'
c
