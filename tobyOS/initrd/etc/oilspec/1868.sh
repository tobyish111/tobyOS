show() {
  echo show

  # bash gives an error here - !hookSlice unbound, even though preHooks exists
  # OSH currently does the "logical" thing

  # NOT testing this -- I think this is WHAT NIX WORKS AROUND WITH
  #argv.py ${!hooksSlice}

  argv.py ${!hooksSlice+"${!hooksSlice}"}
}

hooksSlice='preHooks[@]'

set -u
preHooks=()
show

preHooks=('foo bar' baz)
show
