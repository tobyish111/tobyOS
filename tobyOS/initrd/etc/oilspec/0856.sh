f() {
  . $REPO_ROOT/spec/testdata/source-argv.sh              # no argv
  . $REPO_ROOT/spec/testdata/source-argv.sh args to src  # new argv
  echo $@
  echo foo=$foo  # defined in source-argv.sh
}
f args to func
echo foo=$foo  # not defined
