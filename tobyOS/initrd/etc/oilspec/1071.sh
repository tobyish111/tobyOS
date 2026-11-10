case $SH in dash|zsh) exit ;; esac

# Note: ulimit -n -S 1111 is OK in osh/dash/mksh, but not bash/zsh
# Mus be ulimit -S -n 1111

show_state() {
  local msg=$1
  echo "$msg"
  echo -n '  '; ulimit -S -t
  echo -n '  '; ulimit -H -t
  echo
}

show_state 'init'

ulimit -S -t 123456
show_state '-S'

ulimit -H -t 123457
show_state '-H'

ulimit -t 123455
show_state 'no flag'

echo 'GET'

ulimit -S -t 123454
echo -n '  '; ulimit -t
echo -n '  '; ulimit -S -t
echo -n '  '; ulimit -H -t
