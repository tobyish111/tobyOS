#
# This idiom discussed on
# https://github.com/NixOS/nixpkgs/pull/147629

show() {
  echo show

  # These are actually different
  argv.py ${!hooksSlice}

  argv.py ${!hooksSlice+"${!hooksSlice}"}
}

hooksSlice='preHooks[@]'

preHooks=()
show

preHooks=('foo bar' baz)
show

# WTF this exposes a difference?  But not the test case below?

# What's happening here?
# Uncomment this and get an error in bash about hookSlice, even though we never
# undefined it.

#wtf=1
#
# line 6: !hooksSlice: unbound variable

if test -n "$wtf"; then
  # 4.4.0(1)-release
  # echo $BASH_VERSION

  set -u
  preHooks=()
  show

  preHooks=('foo bar' baz)
  show
fi
