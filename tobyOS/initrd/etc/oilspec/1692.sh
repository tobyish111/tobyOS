cd $REPO_ROOT

f() {
  # Dash splits words and globs before handing it to the 'local' builtin.  But
  # ash doesn't!
  local foo=$1
  echo "$foo"
}
f 'void *'
