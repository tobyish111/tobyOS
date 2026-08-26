case $SH in dash|ash) exit ;; esac

foo() {
  if test -v ARGV; then
    echo 'BUG local'
  fi
  ARGV=( a b )
  echo len=${#ARGV[@]}
}

if test -v ARGV; then
  echo 'BUG global'
fi
foo
