# read -a is used in bash-completion
# none of these shells implement it
case $SH in
  *mksh|*dash|*zsh|*/ash)
    exit 2;
    ;;
esac

read -a myarray <<'EOF'
a b c\ d
EOF
argv.py "${myarray[@]}"

# arguments are ignored here
read -r -a array2 extra arguments <<'EOF'
a b c\ d
EOF
argv.py "${array2[@]}"
argv.py "${extra[@]}"
argv.py "${arguments[@]}"
