profile() {
  echo "profile [$@]"
}
g() {
  echo --
  echo g
  echo --
  return
}
f() {
  echo --
  echo f
  echo --
  g
}
# RETURN trap doesn't fire when a function returns, only when a script returns?
# That's not what the manual says.
trap 'profile x y' RETURN
f
. $REPO_ROOT/spec/testdata/return-helper.sh
