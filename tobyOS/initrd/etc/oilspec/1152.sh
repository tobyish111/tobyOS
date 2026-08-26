show_options() {
  case $- in
    *v*) echo verbose-on ;;
  esac
  case $- in
    *x*) echo xtrace-on ;;
  esac
}

set -x -v
show_options
echo

set - a b c
echo "$@"
show_options
echo

# dash that's not leading is not special
set x - y z
echo "$@"
