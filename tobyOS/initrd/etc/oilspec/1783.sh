export PS1=''

case $SH in
  *bash|*osh)
    $SH --rcfile /dev/null -i << 'EOF'
PROMPT_COMMAND='[[ clobber =~ (.)(.)(.) ]]; echo ---'
echo one
[[ bar =~ (.)(.)(.) ]]
echo ${BASH_REMATCH[@]}
EOF
    ;;
esac

# Paper over difference with OSH
case $SH in *bash) echo '^D' ;; esac
