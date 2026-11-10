case $SH in
  bash) set -o posix ;;
esac

eval 'echo hi'

eval() {
  echo 'sh func' "$@"
}

eval 'echo hi'
