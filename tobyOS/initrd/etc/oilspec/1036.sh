case $SH in ash|dash|mksh|zsh) exit ;; esac

strftime-format() {
  local n=$1

  # Prints increasingly long format strings:
  # %(%Y)T %(%Y)T %(%Y%Y)T ...

  echo -n '%('
  for i in $(seq $n); do
    echo -n '%Y'
  done
  echo -n ')T'
}

printf $(strftime-format 1) | wc --bytes
printf $(strftime-format 10) | wc --bytes
printf $(strftime-format 30) | wc --bytes
printf $(strftime-format 31) | wc --bytes
printf $(strftime-format 32) | wc --bytes

case $SH in
  (*/_bin/cxx-dbg/*)    
    # Ensure that oils-for-unix detects the truncation of a fixed buffer.
    # bash has a buffer of 128.

    set +o errexit
    (
      printf $(strftime-format 1000)
    )
    status=$?
    if test $status -ne 1; then
      echo FAIL
    fi
    ;;
esac
