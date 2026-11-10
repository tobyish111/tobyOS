cd $REPO_ROOT

# Comments on bash quirk:
# https://github.com/oilshell/oil/pull/656#issuecomment-599162211

f() {
  . spec/testdata/echo-funcname.sh
}
g() {
  f
}

g
echo -----

. spec/testdata/echo-funcname.sh
echo -----

argv.py "${FUNCNAME[@]}"

# Show bash inconsistency.  FUNCNAME doesn't behave like a normal array.
case $SH in 
  (bash)
    echo -----
    a=('A')
    argv.py '  @' "${a[@]}"
    argv.py '  0' "${a[0]}"
    argv.py '${}' "${a}"
    argv.py '  $' "$a"
    ;;
esac
