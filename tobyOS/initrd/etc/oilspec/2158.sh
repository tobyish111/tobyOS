# from bash-completion
pat='^(\$\{?)([A-Za-z0-9_]*)$'
[[ '$PA' =~ $pat ]] && argv.py "${BASH_REMATCH[@]}"
